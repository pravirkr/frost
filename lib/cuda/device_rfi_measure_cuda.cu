// Device RFI measurement phases (docs/rfi.md "Dataflow and shared statistics",
// "Broad-band excision", "Replacement"): the remedian bandpass estimate per
// channel per window, the per-sample detection statistic (sdet) and
// per-channel min/max feeding build_flags() (device_rfi_cuda.cu), and the
// FDMT-handoff normalization using the bandpass phase's own loc/scale.
// measure_bandpass mirrors refine_bandpass in lib/cpu/bandpass_kernels.hpp;
// same accumulation order so the two paths agree to float precision.

#include "lib/cuda/device_rfi_detail.cuh"
#include "lib/prepare/bandpass_sampling.hpp"

namespace frost::pipeline::device {

template <DataType D>
__global__ void sample_rows(const uint8_t* in,
                            int stride,
                            int nchans,
                            int base,
                            int len,
                            int m,
                            float* out) {
    const int c = (blockIdx.x * blockDim.x) + threadIdx.x;
    const int i = static_cast<int>(blockIdx.y);
    if (c >= nchans || i >= m) {
        return;
    }
    // 64-bit: i * len overflows int once len exceeds ~687k samples.
    const int t =
        base + static_cast<int>((static_cast<long long>(i) * len) / m);
    out[(static_cast<size_t>(i) * nchans) + c] =
        dev_read<D>(in + (static_cast<size_t>(t) * stride), c);
}

__global__ void remedian5(const float* in, int count, int nchans, float* out) {
    const int i    = (blockIdx.x * blockDim.x) + threadIdx.x;
    const int next = count / 5;
    if (i >= next * nchans) {
        return;
    }
    const int c     = i % nchans;
    const int outer = i / nchans;
    const int start = (5 * outer * nchans) + c;
    out[i] = median5(in[start], in[start + nchans], in[start + (2 * nchans)],
                     in[start + (3 * nchans)], in[start + (4 * nchans)]);
}

__global__ void abs_diff(
    const float* samples, const float* loc, int n, int nchans, float* out) {
    const int i = (blockIdx.x * blockDim.x) + threadIdx.x;
    if (i >= n) {
        return;
    }
    out[i] = fabsf(samples[i] - loc[i % nchans]);
}

__global__ void refine_window(const float* samples,
                              const float* seed,
                              const float* mad,
                              int m,
                              int nchans,
                              float* loc,
                              float* scale,
                              float* inv,
                              int* unmeasured) {
    const int c = (blockIdx.x * blockDim.x) + threadIdx.x;
    if (c >= nchans) {
        return;
    }
    float l       = seed[c];
    float s       = mad[c] * kMadToStd;
    const bool ok = refine_bandpass(samples + c, m, nchans, l, s);
    loc[c]        = l;
    scale[c]      = s;
    if (ok) {
        inv[c] = 1.0F / s;
    } else {
        inv[c] = 0.0F;
        unmeasured[c] += 1;
    }
}

template <DataType D>
void DeviceRfi::Impl::measure_bandpass(cudaStream_t q,
                                       int nsamps,
                                       int nchans,
                                       int row,
                                       prepare::rfi::RfiReport& report) {
    const int nwin    = std::max(nsamps / as_int(m_window_len, "window"), 1);
    const auto band_n = static_cast<std::size_t>(kMaxBandpass) *
                        static_cast<std::size_t>(nchans);
    fit(m_samples, band_n);
    fit(m_work, band_n);
    fit(m_work2, band_n);
    fit(m_loc,
        static_cast<std::size_t>(nwin) * static_cast<std::size_t>(nchans));
    fit(m_scale, m_loc.size());
    fit(m_inv, m_loc.size());
    fit(m_unmeasured, static_cast<std::size_t>(nchans));
    FROST_CUDA_CHECK(
        cudaMemsetAsync(raw(m_unmeasured), 0,
                        sizeof(int) * static_cast<std::size_t>(nchans), q));

    const auto t         = Clock::now();
    const int window_len = as_int(m_window_len, "window");
    for (int w = 0; w < nwin; ++w) {
        const int begin = w * window_len;
        const int len   = (w + 1 == nwin) ? (nsamps - begin) : window_len;
        const int m =
            static_cast<int>(frost::prepare::detail::largest_power_of_five_leq(
                static_cast<SizeType>(std::min(kMaxBandpass, len))));
        if (m < kMinWindow) {
            throw std::invalid_argument(
                "nsamps too small to estimate a bandpass (need >= 5)");
        }
        sample_rows<D><<<dim3(blocks_for(nchans), m), kThreads, 0, q>>>(
            raw(m_bytes), row, nchans, begin, len, m, raw(m_samples));
        check_launch();
        FROST_CUDA_CHECK(
            cudaMemcpyAsync(raw(m_work), raw(m_samples),
                            sizeof(float) * static_cast<std::size_t>(m) *
                                static_cast<std::size_t>(nchans),
                            cudaMemcpyDeviceToDevice, q));
        float* cur = raw(m_work);
        float* alt = raw(m_work2);
        int count  = m;
        while (count > 1) {
            const int next = count / 5;
            remedian5<<<blocks_for(next * nchans), kThreads, 0, q>>>(
                cur, count, nchans, alt);
            check_launch();
            std::swap(cur, alt);
            count = next;
        }
        float* loc_row = raw(m_loc) + (w * nchans);
        FROST_CUDA_CHECK(cudaMemcpyAsync(
            loc_row, cur, sizeof(float) * static_cast<std::size_t>(nchans),
            cudaMemcpyDeviceToDevice, q));
        abs_diff<<<blocks_for(m * nchans), kThreads, 0, q>>>(
            raw(m_samples), loc_row, m * nchans, nchans, raw(m_work));
        check_launch();
        cur   = raw(m_work);
        alt   = raw(m_work2);
        count = m;
        while (count > 1) {
            const int next = count / 5;
            remedian5<<<blocks_for(next * nchans), kThreads, 0, q>>>(
                cur, count, nchans, alt);
            check_launch();
            std::swap(cur, alt);
            count = next;
        }
        refine_window<<<blocks_for(nchans), kThreads, 0, q>>>(
            raw(m_samples), loc_row, cur, m, nchans, loc_row,
            raw(m_scale) + (w * nchans), raw(m_inv) + (w * nchans),
            raw(m_unmeasured));
        check_launch();
    }
    FROST_CUDA_CHECK(cudaStreamSynchronize(q));
    report.t_bandpass = seconds_since(t);
}

template <DataType D>
__global__ void scan_sdet(const uint8_t* in,
                          int stride,
                          int nsamps,
                          int nchans,
                          int window_len,
                          int nwin,
                          float clip,
                          const float* loc_all,
                          const float* inv_all,
                          const uint8_t* mask,
                          float* sdet) {
    const int t = (blockIdx.x * blockDim.x) + threadIdx.x;
    if (t >= nsamps) {
        return;
    }
    int w = t / window_len;
    if (w >= nwin) {
        w = nwin - 1;
    }
    const float* loc   = loc_all + (w * nchans);
    const float* inv   = inv_all + (w * nchans);
    const uint8_t* row = in + (static_cast<size_t>(t) * stride);
    double sum         = 0.0;
    double nmeas       = 0.0;
    for (int c = 0; c < nchans; ++c) {
        if (mask[c] == 0 || inv[c] == 0.0F) {
            continue;
        }
        nmeas += 1.0;
        const float x = dev_read<D>(row, c);
        const float z = (x - loc[c]) * inv[c];
        float cl      = z;
        if (cl > clip) {
            cl = clip;
        }
        if (cl < -clip) {
            cl = -clip;
        }
        sum += static_cast<double>(cl);
    }
    const double dn = nmeas < 1.0 ? 1.0 : nmeas;
    sdet[t]         = static_cast<float>(sum / dn);
}

template <DataType D>
__global__ void channel_minmax(const uint8_t* in,
                               int stride,
                               int nsamps,
                               int nchans,
                               const uint8_t* mask,
                               float* cmin,
                               float* cmax) {
    const int c = (blockIdx.x * blockDim.x) + threadIdx.x;
    if (c >= nchans) {
        return;
    }
    float lo = FLT_MAX;
    float hi = -FLT_MAX;
    if (mask[c] != 0) {
        for (int t = 0; t < nsamps; ++t) {
            const float x =
                dev_read<D>(in + (static_cast<size_t>(t) * stride), c);
            lo = ord_min(lo, x);
            hi = ord_max(hi, x);
        }
    }
    cmin[c] = lo;
    cmax[c] = hi;
}

template <DataType D>
SizeType DeviceRfi::Impl::scan(cudaStream_t q,
                               int nsamps,
                               int nchans,
                               int row,
                               prepare::rfi::RfiReport& report) {
    fit(m_sdet, static_cast<std::size_t>(nsamps));
    fit(m_cmin, static_cast<std::size_t>(nchans));
    fit(m_cmax, static_cast<std::size_t>(nchans));

    const auto t = Clock::now();
    upload_mask(q);
    const SizeType static_good = m_mask_host.size() - count_zeros(m_mask_host);
    const int window_len       = as_int(m_window_len, "window");
    const int nwin             = std::max(nsamps / window_len, 1);
    if (static_good > 0) {
        scan_sdet<D><<<blocks_for(nsamps), kThreads, 0, q>>>(
            raw(m_bytes), row, nsamps, nchans, window_len, nwin, m_clip,
            raw(m_loc), raw(m_inv), raw(m_mask), raw(m_sdet));
        check_launch();
        channel_minmax<D><<<blocks_for(nchans), kThreads, 0, q>>>(
            raw(m_bytes), row, nsamps, nchans, raw(m_mask), raw(m_cmin),
            raw(m_cmax));
        check_launch();
    }
    copy_d2h(m_sdet_host, raw(m_sdet), static_good > 0 ? nsamps : 0, q);
    copy_d2h(m_cmin_host, raw(m_cmin), static_good > 0 ? nchans : 0, q);
    copy_d2h(m_cmax_host, raw(m_cmax), static_good > 0 ? nchans : 0, q);
    copy_d2h(m_unmeasured_host, raw(m_unmeasured), nchans, q);
    FROST_CUDA_CHECK(cudaStreamSynchronize(q));
    report.t_scan = seconds_since(t);
    return static_good;
}

template <DataType D>
__global__ void normalize_kernel(const uint8_t* in,
                                 int stride,
                                 int nchans,
                                 int block_n,
                                 int valid,
                                 int flip,
                                 int window_len,
                                 int nwin,
                                 int calibration_windows,
                                 const uint8_t* mask,
                                 const float* loc,
                                 const float* scale,
                                 float* dest) {
    const int c     = static_cast<int>(blockIdx.x);
    const int dst_c = flip ? nchans - 1 - c : c;
    // Warm short tails retain the preceding window's bandpass and mask.
    // Only a cold start falls back to measuring the short block itself.
    float short_mean = 0.0F, short_std = 0.0F;
    const bool cold = valid < kMinBlockSamps && calibration_windows == 0;
    if (cold) {
        double mean = 0.0, variance = 0.0;
        for (int t = 0; t < valid; ++t) {
            const double x =
                dev_read<D>(in + static_cast<size_t>(t) * stride, c);
            const double delta = x - mean;
            mean += delta / (t + 1);
            variance += delta * (x - mean);
        }
        short_mean = static_cast<float>(mean);
        short_std  = valid ? static_cast<float>(sqrt(variance / valid)) : 0.0F;
    }
    for (int t = static_cast<int>(threadIdx.x); t < block_n; t += blockDim.x) {
        float value = 0.0F;
        if (t < valid && mask[c]) {
            const int w       = valid < kMinBlockSamps && !cold
                                    ? calibration_windows - 1
                                    : min(t / window_len, nwin - 1);
            const float mean  = cold ? short_mean : loc[w * nchans + c];
            const float sigma = cold ? short_std : scale[w * nchans + c];
            const float x =
                dev_read<D>(in + static_cast<size_t>(t) * stride, c);
            value = (x - mean) * (sigma > 0.0F ? 1.0F / sigma : 1.0F);
        }
        dest[static_cast<size_t>(dst_c) * block_n + t] = value;
    }
}

dmt::DeviceSpan<const float>
DeviceRfi::Impl::normalize_to_fdmt(SizeType nchans,
                                   SizeType nsamps,
                                   SizeType valid,
                                   bool flip,
                                   dmt::Stream stream) {
    if (!m_have_block || nchans != m_layout.nchans || nsamps != m_block ||
        valid != m_nsamps) {
        throw std::invalid_argument(
            "device normalization does not match the cleaned block");
    }
    FROST_CUDA_CHECK(cudaSetDevice(m_device_id));
    fit(m_normalized, nchans * nsamps);
    const auto q      = queue_of(stream);
    const auto launch = [&]<DataType D>() {
        normalize_kernel<D><<<as_int(nchans, "nchans"), kThreads, 0, q>>>(
            raw(m_bytes), as_int(m_row_bytes, "row"), as_int(nchans, "nchans"),
            as_int(nsamps, "nsamps"), as_int(valid, "valid"), flip ? 1 : 0,
            as_int(m_window_len, "window"),
            std::max(as_int(valid / m_window_len, "windows"), 1),
            as_int(m_calibration_windows, "calibration windows"), raw(m_mask),
            raw(m_loc), raw(m_scale), raw(m_normalized));
    };
    switch (m_layout.dtype) {
    case DataType::kUInt1:
        launch.template operator()<DataType::kUInt1>();
        break;
    case DataType::kUInt2:
        launch.template operator()<DataType::kUInt2>();
        break;
    case DataType::kUInt4:
        launch.template operator()<DataType::kUInt4>();
        break;
    case DataType::kUInt8:
        launch.template operator()<DataType::kUInt8>();
        break;
    case DataType::kUInt16:
        launch.template operator()<DataType::kUInt16>();
        break;
    case DataType::kUInt32:
        launch.template operator()<DataType::kUInt32>();
        break;
    case DataType::kFloat32:
        throw std::invalid_argument("device RFI is integer-only");
    }
    check_launch();
    return {raw(m_normalized), nchans * nsamps, {gpu_backend(), m_device_id}};
}

#define FROST_RFI_INSTANTIATE_MEASURE(d)                                       \
    template void DeviceRfi::Impl::measure_bandpass<d>(                        \
        cudaStream_t, int, int, int, prepare::rfi::RfiReport&);                \
    template SizeType DeviceRfi::Impl::scan<d>(cudaStream_t, int, int, int,    \
                                               prepare::rfi::RfiReport&);
FROST_RFI_FOR_EACH_DEVICE_CODEC(FROST_RFI_INSTANTIATE_MEASURE)
#undef FROST_RFI_INSTANTIATE_MEASURE

} // namespace frost::pipeline::device
