// Device RFI core, flags and apply: construction, validation, the
// per-block clean() dispatcher, the host-only flag combination phase
// (docs/rfi.md "Dead channels", "Broad-band excision"), the neighbour-fill
// apply phase (docs/rfi.md "Replacement"), and the public
// DeviceRfi/WaterfallBuffer wrappers. Bandpass/scan measurement and
// device normalization live in the sibling device_rfi_measure_cuda.cu;
// see lib/cuda/device_rfi_detail.cuh for why they stay split that way.

#include "lib/cuda/device_rfi_detail.cuh"
#include "lib/prepare/rfi_validation.hpp"

namespace frost::pipeline::device {

DeviceRfi::Impl::Impl(dmt::Backend backend,
                      int device_id,
                      prepare::rfi::RfiConfig cfg,
                      const prepare::rfi::BlockLayout& layout,
                      float baseline_length)
    : m_device_id(device_id),
      m_cfg(std::move(cfg)),
      m_layout(layout) {
#if defined(FROST_ENABLE_CUDA)
    if (backend != dmt::Backend::kCUDA) {
        throw std::invalid_argument(
            "DeviceRfi was built for CUDA and was given another backend");
    }
#elif defined(FROST_ENABLE_HIP)
    if (backend != dmt::Backend::kHIP) {
        throw std::invalid_argument(
            "DeviceRfi was built for HIP and was given another backend");
    }
#else
    throw std::invalid_argument("DeviceRfi requires a CUDA or HIP build");
#endif
    if (device_id < 0) {
        throw std::invalid_argument("device_id must be >= 0");
    }
    validate(baseline_length);
    FROST_CUDA_CHECK(cudaSetDevice(device_id));
    const double window_samples =
        static_cast<double>(baseline_length) / layout.tsamp;
    m_window_len = std::max(static_cast<SizeType>(window_samples),
                            static_cast<SizeType>(kMinWindow));
    m_smooth_radius =
        std::max<SizeType>(static_cast<SizeType>(window_samples / 2), 1);
    m_time_radius = std::max<SizeType>(m_smooth_radius / 4, 1);
    m_chan_radius = std::max<SizeType>(layout.nchans / 60, 1);
    m_narrow      = m_cfg.narrow.enabled && bits_of(layout.dtype) > 4;
    m_clip        = m_cfg.narrow.threshold;
    m_row_bytes =
        layout.nchans * static_cast<SizeType>(bits_of(layout.dtype)) / 8;
    build_static_mask();
}

RfiDeviceOutput DeviceRfi::Impl::execute(std::span<const uint8_t> host,
                                         SizeType stride,
                                         SizeType nsamps,
                                         SizeType block,
                                         dmt::Stream stream) {
    if (stride != m_row_bytes) {
        throw std::invalid_argument("device RFI requires a tightly packed row "
                                    "(stride == nchans * bits / 8)");
    }
    if (nsamps > m_layout.max_nsamps) {
        throw std::invalid_argument(
            "device RFI block exceeds the layout's max_nsamps");
    }
    if (block < nsamps || block == 0) {
        throw std::invalid_argument(
            "device RFI block must cover the real samples");
    }
    if (host.size() < nsamps * stride) {
        throw std::invalid_argument("device RFI input span is short");
    }
    FROST_CUDA_CHECK(cudaSetDevice(m_device_id));
    const auto q      = queue_of(stream);
    const int i_ns    = as_int(nsamps, "nsamps");
    const int i_block = as_int(block, "block");
    const int i_nch   = as_int(m_layout.nchans, "nchans");
    const int i_row   = as_int(m_row_bytes, "row bytes");
    const auto bytes =
        static_cast<std::size_t>(i_block) * static_cast<std::size_t>(i_row);
    fit(m_bytes, bytes);
    FROST_CUDA_CHECK(cudaMemsetAsync(raw(m_bytes), 0, bytes, q));
    if (i_ns > 0 && !host.empty()) {
        FROST_CUDA_CHECK(cudaMemcpyAsync(raw(m_bytes), host.data(),
                                         static_cast<std::size_t>(i_ns) *
                                             static_cast<std::size_t>(i_row),
                                         cudaMemcpyHostToDevice, q));
    }
    m_have_block = true;
    m_block      = block;
    m_nsamps     = nsamps;

    prepare::rfi::RfiReport report;
    report.nsamps       = nsamps;
    report.chans_zapped = count_zeros(m_static);
    report.chans_masked = report.chans_zapped;
    if (i_ns < std::max(kMinWindow, kMinBlockSamps)) {
        if (m_calibration_windows == 0) {
            m_mask_host = m_static;
        } else {
            report.chans_masked       = count_zeros(m_mask_host);
            report.calibration_reused = true;
        }
        ++m_block_index;
        FROST_CUDA_CHECK(cudaStreamSynchronize(q));
        upload_mask(q);
        return {m_mask_host, report};
    }
    m_calibration_windows = 0;
    m_mask_host           = m_static;
    switch (m_layout.dtype) {
    case DataType::kUInt1:
        clean<DataType::kUInt1>(q, i_ns, i_block, i_nch, i_row, report);
        break;
    case DataType::kUInt2:
        clean<DataType::kUInt2>(q, i_ns, i_block, i_nch, i_row, report);
        break;
    case DataType::kUInt4:
        clean<DataType::kUInt4>(q, i_ns, i_block, i_nch, i_row, report);
        break;
    case DataType::kUInt8:
        clean<DataType::kUInt8>(q, i_ns, i_block, i_nch, i_row, report);
        break;
    case DataType::kUInt16:
        clean<DataType::kUInt16>(q, i_ns, i_block, i_nch, i_row, report);
        break;
    case DataType::kUInt32:
        clean<DataType::kUInt32>(q, i_ns, i_block, i_nch, i_row, report);
        break;
    case DataType::kFloat32:
        throw std::invalid_argument(
            "device RFI integer path does not run on float samples");
    }
    m_calibration_windows = std::max<SizeType>(nsamps / m_window_len, 1);
    ++m_block_index;
    return {m_mask_host, report};
}

void DeviceRfi::Impl::download(std::span<uint8_t> host,
                               SizeType stride,
                               SizeType nsamps,
                               dmt::Stream stream) const {
    if (!m_have_block || stride != m_row_bytes || nsamps > m_block) {
        throw std::invalid_argument("device RFI download does not match "
                                    "the last cleaned block");
    }
    if (host.size() < nsamps * stride) {
        throw std::invalid_argument("device RFI download span is short");
    }
    const auto q = queue_of(stream);
    FROST_CUDA_CHECK(cudaMemcpyAsync(host.data(), raw(m_bytes), nsamps * stride,
                                     cudaMemcpyDeviceToHost, q));
    FROST_CUDA_CHECK(cudaStreamSynchronize(q));
}

SizeType DeviceRfi::Impl::memory_usage() const noexcept {
    return m_bytes.capacity() * sizeof(uint8_t) +
           m_orig.capacity() * sizeof(uint8_t) +
           m_normalized.capacity() * sizeof(float) +
           m_samples.capacity() * sizeof(float) +
           m_work.capacity() * sizeof(float) +
           m_work2.capacity() * sizeof(float) +
           m_loc.capacity() * sizeof(float) +
           m_scale.capacity() * sizeof(float) +
           m_inv.capacity() * sizeof(float) +
           m_unmeasured.capacity() * sizeof(int) +
           m_sdet.capacity() * sizeof(float) +
           m_cmin.capacity() * sizeof(float) +
           m_cmax.capacity() * sizeof(float) +
           m_mask.capacity() * sizeof(uint8_t) +
           m_time_flags.capacity() * sizeof(uint8_t) +
           m_counters.capacity() * sizeof(int);
}

void DeviceRfi::Impl::upload_mask(cudaStream_t q) {
    fit(m_mask, m_mask_host.size());
    if (m_mask_host.empty()) {
        return;
    }
    FROST_CUDA_CHECK(cudaMemcpyAsync(raw(m_mask), m_mask_host.data(),
                                     m_mask_host.size(), cudaMemcpyHostToDevice,
                                     q));
}

template <DataType D>
void DeviceRfi::Impl::clean(cudaStream_t q,
                            int nsamps,
                            int block,
                            int nchans,
                            int row,
                            prepare::rfi::RfiReport& report) {
    (void)block;
    measure_bandpass<D>(q, nsamps, nchans, row, report);
    const SizeType static_good = scan<D>(q, nsamps, nchans, row, report);

    std::vector<uint8_t> time_flags;
    build_flags(nsamps, nchans, static_good, time_flags, report);

    apply<D>(q, nsamps, nchans, row, time_flags, report);
}

void DeviceRfi::Impl::validate(float baseline_length) const {
    m_cfg.validate();
    prepare::rfi::detail::validate_layout(m_layout, baseline_length);
    const auto& l = m_layout;
    if (is_floating(l.dtype)) {
        throw std::invalid_argument(
            "DeviceRfi integer path does not run on float samples");
    }
    if (!m_cfg.zap_freqs.empty() && l.df == 0.0) {
        throw std::invalid_argument(
            "DeviceRfi: zap_freqs needs the channel spacing (df)");
    }
}

void DeviceRfi::Impl::build_static_mask() {
    m_static.assign(m_layout.nchans, 1);
    prepare::rfi::apply_manual_killmask(m_static, m_cfg.zap_chans);
    for (SizeType c = 0; c < m_layout.nchans; ++c) {
        const double f = m_layout.f0 + (static_cast<double>(c) * m_layout.df);
        for (const auto& r : m_cfg.zap_freqs) {
            if (f >= r.lo && f <= r.hi) {
                m_static[c] = 0;
            }
        }
    }
    m_mask_host = m_static;
}

void DeviceRfi::Impl::build_flags(int nsamps,
                                  int nchans,
                                  SizeType static_good,
                                  std::vector<uint8_t>& time_flags,
                                  prepare::rfi::RfiReport& report) {
    const auto t   = Clock::now();
    const int nwin = std::max(nsamps / as_int(m_window_len, "window"), 1);
    if (static_good > 0) {
        for (int c = 0; c < nchans; ++c) {
            if (m_mask_host[static_cast<std::size_t>(c)] == 0 ||
                m_unmeasured_host[static_cast<std::size_t>(c)] < nwin) {
                continue;
            }
            if (!(m_cmax_host[static_cast<std::size_t>(c)] >
                  m_cmin_host[static_cast<std::size_t>(c)])) {
                m_mask_host[static_cast<std::size_t>(c)] = 0;
                ++report.chans_dead;
            }
        }
    }
    report.chans_masked = count_zeros(m_mask_host);
    time_flags.assign(static_cast<std::size_t>(nsamps), 0);
    if (static_good > 0 && m_cfg.broad.enabled) {
        report.samps_broad = broadband_time_flags(
            m_sdet_host, m_smooth_radius, m_cfg.broad.threshold,
            m_cfg.broad.boxcar_max, time_flags);
    }
    report.samps_flagged = static_cast<SizeType>(std::count(
        time_flags.begin(), time_flags.end(), static_cast<uint8_t>(1)));
    report.t_flags       = seconds_since(t);
}

template <DataType D>
__global__ void apply_unflagged(uint8_t* bytes,
                                const uint8_t* orig,
                                int stride,
                                int nsamps,
                                int nchans,
                                int window_len,
                                int nwin,
                                float clip,
                                int narrow,
                                int chan_radius,
                                float max_value,
                                uint64_t seed,
                                uint64_t block_index,
                                const float* loc_all,
                                const float* scale_all,
                                const float* inv_all,
                                const uint8_t* mask,
                                const uint8_t* time_flags,
                                int* cells_narrow,
                                int* fallbacks) {
    const int t = (blockIdx.x * blockDim.x) + threadIdx.x;
    if (t >= nsamps || time_flags[t] != 0) {
        return;
    }
    int w = t / window_len;
    if (w >= nwin) {
        w = nwin - 1;
    }
    const float* loc   = loc_all + (w * nchans);
    const float* scale = scale_all + (w * nchans);
    const float* inv   = inv_all + (w * nchans);
    uint8_t* out       = bytes + (static_cast<size_t>(t) * stride);
    // Classify and draw sources from the untouched snapshot: the host
    // (RFICleaner::apply) decides every cell of a row from the original
    // values before writing any of them, so a neighbour that was already
    // refilled must not be read back here.
    const uint8_t* src_row = orig + (static_cast<size_t>(t) * stride);
    for (int c = 0; c < nchans; ++c) {
        if (mask[c] == 0) {
            dev_write<D>(out, c,
                         dev_quantise(loc[c] + (scale[c] * 0.0F), max_value));
            continue;
        }
        if (inv[c] == 0.0F) {
            continue;
        }
        const float z = (dev_read<D>(src_row, c) - loc[c]) * inv[c];
        if (!(narrow != 0 && fabsf(z) > clip)) {
            continue;
        }
        atomicAdd(cells_narrow, 1);
        const int lo = c > chan_radius ? c - chan_radius : 0;
        int hi       = c + chan_radius;
        if (hi > nchans - 1) {
            hi = nchans - 1;
        }
        float value = 0.0F;
        bool found  = false;
        for (int a = 0; a < kMaxDraws && !found; ++a) {
            const uint64_t h =
                hash5(seed, block_index,
                      (kStreamCell << 56U) ^ static_cast<uint64_t>(t),
                      static_cast<uint64_t>(c), static_cast<uint64_t>(a));
            const int src = uniform_index(h, lo, hi);
            if (!is_clean<D>(src_row, src, loc, inv, mask, narrow, clip)) {
                continue;
            }
            value = (dev_read<D>(src_row, src) - loc[src]) * inv[src];
            found = true;
        }
        if (!found) {
            atomicAdd(fallbacks, 1);
        }
        dev_write<D>(out, c,
                     dev_quantise(loc[c] + (scale[c] * value), max_value));
    }
}

template <DataType D>
__global__ void apply_flagged(uint8_t* bytes,
                              int stride,
                              int nsamps,
                              int nchans,
                              int window_len,
                              int nwin,
                              int time_radius,
                              float max_value,
                              uint64_t seed,
                              uint64_t block_index,
                              const float* loc_all,
                              const float* scale_all,
                              const uint8_t* mask,
                              const uint8_t* time_flags,
                              int* fallbacks) {
    const int t = (blockIdx.x * blockDim.x) + threadIdx.x;
    if (t >= nsamps || time_flags[t] == 0) {
        return;
    }
    int w = t / window_len;
    if (w >= nwin) {
        w = nwin - 1;
    }
    const float* loc   = loc_all + (w * nchans);
    const float* scale = scale_all + (w * nchans);
    uint8_t* out       = bytes + (static_cast<size_t>(t) * stride);
    const int lo       = t > time_radius ? t - time_radius : 0;
    int hi             = t + time_radius;
    if (hi > nsamps - 1) {
        hi = nsamps - 1;
    }
    for (int c = 0; c < nchans; ++c) {
        if (mask[c] == 0) {
            dev_write<D>(out, c,
                         dev_quantise(loc[c] + (scale[c] * 0.0F), max_value));
            continue;
        }
        bool found = false;
        for (int a = 0; a < kMaxDraws && !found; ++a) {
            const uint64_t h =
                hash5(seed, block_index,
                      (kStreamTime << 56U) ^ static_cast<uint64_t>(t),
                      static_cast<uint64_t>(c), static_cast<uint64_t>(a));
            const int src = uniform_index(h, lo, hi);
            if (time_flags[src] != 0) {
                continue;
            }
            dev_copy<D>(bytes + (static_cast<size_t>(src) * stride), out, c);
            found = true;
        }
        if (!found) {
            atomicAdd(fallbacks, 1);
            dev_write<D>(out, c,
                         dev_quantise(loc[c] + (scale[c] * 0.0F), max_value));
        }
    }
}

template <DataType D>
void DeviceRfi::Impl::apply(cudaStream_t q,
                            int nsamps,
                            int nchans,
                            int row,
                            const std::vector<uint8_t>& time_flags,
                            prepare::rfi::RfiReport& report) {
    const auto t         = Clock::now();
    const int window_len = as_int(m_window_len, "window");
    const int nwin       = std::max(nsamps / window_len, 1);
    fit(m_time_flags, time_flags.size());
    fit(m_counters, 2);
    upload_mask(q);
    FROST_CUDA_CHECK(cudaMemcpyAsync(raw(m_time_flags), time_flags.data(),
                                     time_flags.size(), cudaMemcpyHostToDevice,
                                     q));
    FROST_CUDA_CHECK(cudaMemsetAsync(raw(m_counters), 0, sizeof(int) * 2, q));
    const float max_value = prepare::rfi::kernels::Codec<D>::max_value();
    fit(m_orig, m_bytes.size());
    FROST_CUDA_CHECK(cudaMemcpyAsync(raw(m_orig), raw(m_bytes), m_bytes.size(),
                                     cudaMemcpyDeviceToDevice, q));
    apply_unflagged<D><<<blocks_for(nsamps), kThreads, 0, q>>>(
        raw(m_bytes), raw(m_orig), row, nsamps, nchans, window_len, nwin,
        m_clip, m_narrow ? 1 : 0, as_int(m_chan_radius, "chan radius"),
        max_value, m_cfg.seed, m_block_index, raw(m_loc), raw(m_scale),
        raw(m_inv), raw(m_mask), raw(m_time_flags), raw(m_counters),
        raw(m_counters) + 1);
    check_launch();
    apply_flagged<D><<<blocks_for(nsamps), kThreads, 0, q>>>(
        raw(m_bytes), row, nsamps, nchans, window_len, nwin,
        as_int(m_time_radius, "time radius"), max_value, m_cfg.seed,
        m_block_index, raw(m_loc), raw(m_scale), raw(m_mask), raw(m_time_flags),
        raw(m_counters) + 1);
    check_launch();
    int counters[2] = {0, 0};
    FROST_CUDA_CHECK(cudaMemcpyAsync(counters, raw(m_counters),
                                     sizeof(counters), cudaMemcpyDeviceToHost,
                                     q));
    FROST_CUDA_CHECK(cudaStreamSynchronize(q));
    report.cells_narrow   = static_cast<SizeType>(counters[0]);
    report.fill_fallbacks = static_cast<SizeType>(counters[1]);
    report.t_apply        = seconds_since(t);
}

SizeType DeviceRfi::memory_usage() const noexcept {
    return m_impl->memory_usage();
}

DeviceRfi::DeviceRfi(dmt::Backend backend,
                     int device_id,
                     prepare::rfi::RfiConfig cfg,
                     const prepare::rfi::BlockLayout& layout,
                     float baseline_length)
    : m_impl(std::make_unique<Impl>(
          backend, device_id, std::move(cfg), layout, baseline_length)) {}

DeviceRfi::~DeviceRfi()                               = default;
DeviceRfi::DeviceRfi(DeviceRfi&&) noexcept            = default;
DeviceRfi& DeviceRfi::operator=(DeviceRfi&&) noexcept = default;

RfiDeviceOutput DeviceRfi::execute(std::span<const uint8_t> host,
                                   SizeType stride,
                                   SizeType nsamps,
                                   SizeType block,
                                   dmt::Stream stream) {
    return m_impl->execute(host, stride, nsamps, block, stream);
}

void DeviceRfi::download(std::span<uint8_t> host,
                         SizeType stride,
                         SizeType nsamps,
                         dmt::Stream stream) const {
    m_impl->download(host, stride, nsamps, stream);
}

dmt::DeviceSpan<const float> DeviceRfi::normalize_to_fdmt(SizeType nchans,
                                                          SizeType nsamps,
                                                          SizeType valid_nsamps,
                                                          bool flip_chans,
                                                          dmt::Stream stream) {
    return m_impl->normalize_to_fdmt(nchans, nsamps, valid_nsamps, flip_chans,
                                     stream);
}

SizeType available_memory(dmt::Backend backend, int device_id) {
    if (backend != gpu_backend())
        throw std::invalid_argument("unavailable GPU backend");
    int previous = 0;
    FROST_CUDA_CHECK(cudaGetDevice(&previous));
    FROST_CUDA_CHECK(cudaSetDevice(device_id));
    std::size_t free = 0, total = 0;
    const auto status = cudaMemGetInfo(&free, &total);
    FROST_CUDA_CHECK(cudaSetDevice(previous));
    FROST_CUDA_CHECK(status);
    return free;
}

class WaterfallBuffer::Impl {
public:
    Impl(dmt::Backend backend, int device_id, SizeType capacity)
        : device{backend, device_id},
          capacity(capacity) {
        if (backend != gpu_backend())
            throw std::invalid_argument("unavailable GPU backend");
        FROST_CUDA_CHECK(cudaSetDevice(device_id));
        FROST_CUDA_CHECK(cudaStreamCreate(&queue));
    }
    ~Impl() {
        cudaSetDevice(device.id);
        cudaStreamSynchronize(queue);
        cudaStreamDestroy(queue);
    }
    dmt::Device device;
    SizeType capacity;
    cudaStream_t queue{};
    gpu::DeviceVector<float> buffer;
};
WaterfallBuffer::WaterfallBuffer(dmt::Backend backend,
                                 int device_id,
                                 SizeType capacity)
    : m_impl(std::make_unique<Impl>(backend, device_id, capacity)) {}
WaterfallBuffer::~WaterfallBuffer()                          = default;
WaterfallBuffer::WaterfallBuffer(WaterfallBuffer&&) noexcept = default;
WaterfallBuffer&
WaterfallBuffer::operator=(WaterfallBuffer&&) noexcept = default;
SizeType WaterfallBuffer::memory_usage() const noexcept {
    return m_impl->buffer.capacity() * sizeof(float);
}
dmt::Stream WaterfallBuffer::stream() const { return {m_impl->queue}; }
dmt::DeviceSpan<const float>
WaterfallBuffer::upload(std::span<const float> waterfall) {
    if (waterfall.size() > m_impl->capacity)
        throw std::invalid_argument("waterfall exceeds device buffer");
    FROST_CUDA_CHECK(cudaSetDevice(m_impl->device.id));
    fit(m_impl->buffer, waterfall.size());
    FROST_CUDA_CHECK(cudaMemcpyAsync(raw(m_impl->buffer), waterfall.data(),
                                     waterfall.size_bytes(),
                                     cudaMemcpyHostToDevice, m_impl->queue));
    return {raw(m_impl->buffer), waterfall.size(), m_impl->device};
}

#define FROST_RFI_INSTANTIATE_APPLY(d)                                         \
    template void DeviceRfi::Impl::apply<d>(cudaStream_t, int, int, int,       \
                                            const std::vector<uint8_t>&,       \
                                            prepare::rfi::RfiReport&);
FROST_RFI_FOR_EACH_DEVICE_CODEC(FROST_RFI_INSTANTIATE_APPLY)
#undef FROST_RFI_INSTANTIATE_APPLY

} // namespace frost::pipeline::device
