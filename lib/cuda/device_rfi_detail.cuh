#pragma once

// Shared declarations for the device RFI split (lib/cuda/device_rfi_*.cu).
// Included only from device translation units.
//
// This project does not enable CUDA relocatable device code (no -rdc=true),
// so a __global__ kernel can only be launched from the translation unit
// that defines it, and a __device__ helper called from kernels in more
// than one .cu file must be defined here (inline) so every TU gets its
// own copy. Each phase file therefore keeps its kernels, its host launch
// wrapper, and the Impl method that drives them together; other files
// call only that method, declared below, never the kernel itself.

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <frost/common/numeric.hpp>

#include "lib/cuda/gpu_utils.cuh"
#include "lib/prepare/device_rfi.hpp"
#include "lib/prepare/sample_codec.hpp"

namespace frost::pipeline::device {

using Clock = std::chrono::steady_clock;

constexpr dmt::Backend gpu_backend() {
#ifdef FROST_ENABLE_HIP
    return dmt::Backend::kHIP;
#else
    return dmt::Backend::kCUDA;
#endif
}

constexpr int kThreads     = 128;
constexpr int kMaxBandpass = 3125;
constexpr int kMinWindow   = 5;
// Same floor as RFICleaner: tail blocks this short pass through untouched.
constexpr int kMinBlockSamps   = 64;
constexpr int kMaxDraws        = 10;
constexpr float kMadToStd      = 1.4826F;
constexpr double kClipSigma    = 4.5;
constexpr int kClipIters       = 2;
constexpr uint64_t kStreamCell = 1;
constexpr uint64_t kStreamTime = 2;

inline double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

inline int as_int(SizeType n, const char* what) {
    if (n > static_cast<SizeType>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument(std::string(what) + " does not fit in int");
    }
    return static_cast<int>(n);
}

template <typename T> T* raw(gpu::DeviceVector<T>& vec) {
    return thrust::raw_pointer_cast(vec.data());
}
template <typename T> const T* raw(const gpu::DeviceVector<T>& vec) {
    return thrust::raw_pointer_cast(vec.data());
}

inline int blocks_for(int n) {
    return n <= 0 ? 0 : (n + kThreads - 1) / kThreads;
}

inline cudaStream_t queue_of(dmt::Stream stream) {
    if (stream.native == nullptr) {
        throw std::invalid_argument(
            "device RFI requires the pipeline stream, not the null stream");
    }
    return static_cast<cudaStream_t>(stream.native);
}

inline void check_launch() { FROST_CUDA_CHECK(cudaGetLastError()); }

inline SizeType count_zeros(const std::vector<uint8_t>& v) {
    return static_cast<SizeType>(
        std::count(v.begin(), v.end(), static_cast<uint8_t>(0)));
}

template <typename T> void fit(gpu::DeviceVector<T>& vec, std::size_t n) {
    if (vec.size() < n) {
        vec.resize(n);
    }
}

// ---------------------------------------------------------------------
// Device-side helpers shared by kernels in more than one phase file.
// Each is `inline`/a function template so every including TU compiles
// its own copy; none of these are launched directly as kernels.
// ---------------------------------------------------------------------

__host__ __device__ inline float ord_min(float a, float b) {
    return b < a ? b : a;
}
__host__ __device__ inline float ord_max(float a, float b) {
    return a < b ? b : a;
}

__host__ __device__ inline float median3(float a, float b, float c) {
    return a < b ? (b < c ? b : (a < c ? c : a))
                 : (a < c ? a : (b < c ? c : b));
}

__host__ __device__ inline float
median5(float a, float b, float c, float d, float e) {
    const float f = ord_max(ord_min(a, b), ord_min(c, d));
    const float g = ord_min(ord_max(a, b), ord_max(c, d));
    return median3(e, f, g);
}

__host__ __device__ inline uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31U);
}

__host__ __device__ inline uint64_t
hash5(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    return splitmix64(
        splitmix64(splitmix64(splitmix64(splitmix64(a) ^ b) ^ c) ^ d) ^ e);
}

__host__ __device__ inline int uniform_index(uint64_t h, int lo, int hi) {
    return lo + static_cast<int>(h % static_cast<uint64_t>(hi - lo + 1));
}

template <DataType D> __host__ __device__ constexpr int sample_bits() {
    if constexpr (D == DataType::kUInt1) {
        return 1;
    } else if constexpr (D == DataType::kUInt2) {
        return 2;
    } else if constexpr (D == DataType::kUInt4) {
        return 4;
    } else if constexpr (D == DataType::kUInt8) {
        return 8;
    } else if constexpr (D == DataType::kUInt16) {
        return 16;
    } else {
        return 32;
    }
}

template <DataType D>
__device__ inline float dev_read(const uint8_t* row, int c) {
    if constexpr (D == DataType::kUInt8) {
        return static_cast<float>(row[c]);
    } else if constexpr (D == DataType::kUInt16) {
        const unsigned lo = row[c * 2];
        const unsigned hi = row[(c * 2) + 1];
        return static_cast<float>(lo | (hi << 8U));
    } else if constexpr (D == DataType::kUInt32) {
        unsigned v = row[c * 4];
        v |= static_cast<unsigned>(row[(c * 4) + 1]) << 8U;
        v |= static_cast<unsigned>(row[(c * 4) + 2]) << 16U;
        v |= static_cast<unsigned>(row[(c * 4) + 3]) << 24U;
        return static_cast<float>(v);
    } else {
        constexpr int kBits      = sample_bits<D>();
        constexpr unsigned kMask = (1U << kBits) - 1U;
        const int bit            = c * kBits;
        const unsigned shift     = static_cast<unsigned>(bit) & 7U;
        return static_cast<float>((row[bit >> 3] >> shift) & kMask);
    }
}

template <DataType D>
__device__ inline void dev_write(uint8_t* row, int c, float v) {
    if constexpr (D == DataType::kUInt8) {
        row[c] = static_cast<uint8_t>(v);
    } else if constexpr (D == DataType::kUInt16) {
        const auto raw   = static_cast<unsigned>(v);
        row[c * 2]       = static_cast<uint8_t>(raw & 0xFFU);
        row[(c * 2) + 1] = static_cast<uint8_t>((raw >> 8U) & 0xFFU);
    } else if constexpr (D == DataType::kUInt32) {
        const auto raw   = static_cast<unsigned>(v);
        row[c * 4]       = static_cast<uint8_t>(raw & 0xFFU);
        row[(c * 4) + 1] = static_cast<uint8_t>((raw >> 8U) & 0xFFU);
        row[(c * 4) + 2] = static_cast<uint8_t>((raw >> 16U) & 0xFFU);
        row[(c * 4) + 3] = static_cast<uint8_t>((raw >> 24U) & 0xFFU);
    } else {
        constexpr int kBits      = sample_bits<D>();
        constexpr unsigned kMask = (1U << kBits) - 1U;
        const int bit            = c * kBits;
        const unsigned shift     = static_cast<unsigned>(bit) & 7U;
        auto& byte               = row[bit >> 3];
        byte =
            static_cast<uint8_t>((byte & ~(kMask << shift)) |
                                 ((static_cast<unsigned>(v) & kMask) << shift));
    }
}

template <DataType D>
__device__ inline void dev_copy(const uint8_t* src, uint8_t* dst, int c) {
    if constexpr (D == DataType::kUInt8) {
        dst[c] = src[c];
    } else if constexpr (D == DataType::kUInt16) {
        dst[c * 2]       = src[c * 2];
        dst[(c * 2) + 1] = src[(c * 2) + 1];
    } else if constexpr (D == DataType::kUInt32) {
        dst[c * 4]       = src[c * 4];
        dst[(c * 4) + 1] = src[(c * 4) + 1];
        dst[(c * 4) + 2] = src[(c * 4) + 2];
        dst[(c * 4) + 3] = src[(c * 4) + 3];
    } else {
        dev_write<D>(dst, c, dev_read<D>(src, c));
    }
}

__device__ inline float dev_quantise(float v, float max_value) {
    float q = nearbyintf(v);
    if (q < 0.0F) {
        q = 0.0F;
    }
    if (q > max_value) {
        q = max_value;
    }
    return q;
}

// Same double clip loop as refine_bandpass in lib/cpu/rfi_cpu_measure.cpp.
// One thread owns the channel, so the accumulation order matches the host.
__device__ inline bool
refine_bandpass(const float* v, int m, int stride, float& loc, float& scale) {
    float lo = v[0];
    float hi = v[0];
    for (int i = 1; i < m; ++i) {
        const float x = v[i * stride];
        lo            = ord_min(lo, x);
        hi            = ord_max(hi, x);
    }
    if (!(hi > lo)) {
        loc   = lo;
        scale = 0.0F;
        return false;
    }
    double centre = static_cast<double>(loc);
    double s      = static_cast<double>(scale);
    if (!(s > 0.0)) {
        double mean = 0.0;
        for (int i = 0; i < m; ++i) {
            mean += static_cast<double>(v[i * stride]);
        }
        mean /= static_cast<double>(m);
        double ss = 0.0;
        for (int i = 0; i < m; ++i) {
            const double d = static_cast<double>(v[i * stride]) - mean;
            ss += d * d;
        }
        s = sqrt(ss / static_cast<double>(m));
    }
    for (int it = 0; it < kClipIters; ++it) {
        const double cut = kClipSigma * s;
        double count     = 0.0;
        double sum       = 0.0;
        for (int i = 0; i < m; ++i) {
            const double d = static_cast<double>(v[i * stride]) - centre;
            if (fabs(d) <= cut) {
                count += 1.0;
                sum += d;
            }
        }
        if (count < 2.0) {
            break;
        }
        const double shift = sum / count;
        double ss          = 0.0;
        for (int i = 0; i < m; ++i) {
            const double d = static_cast<double>(v[i * stride]) - centre;
            if (fabs(d) <= cut) {
                const double e = d - shift;
                ss += e * e;
            }
        }
        const double clipped = sqrt(ss / count);
        if (!(clipped > 0.0)) {
            break;
        }
        centre += shift;
        s = clipped;
    }
    loc   = static_cast<float>(centre);
    scale = static_cast<float>(s);
    return true;
}

template <DataType D>
__device__ inline bool is_clean(const uint8_t* row,
                                int c,
                                const float* loc,
                                const float* inv,
                                const uint8_t* mask,
                                int narrow,
                                float clip) {
    if (mask[c] == 0 || inv[c] == 0.0F) {
        return false;
    }
    const float z = (dev_read<D>(row, c) - loc[c]) * inv[c];
    return !(narrow != 0 && fabsf(z) > clip);
}

// Instantiates F(kUInt1) .. F(kUInt32) -- the device RFI path is integer
// only (kFloat32 throws at dispatch, see DeviceRfi::Impl::execute).
#define FROST_RFI_FOR_EACH_DEVICE_CODEC(F)                                     \
    F(DataType::kUInt1)                                                        \
    F(DataType::kUInt2)                                                        \
    F(DataType::kUInt4)                                                        \
    F(DataType::kUInt8)                                                        \
    F(DataType::kUInt16)                                                       \
    F(DataType::kUInt32)

class DeviceRfi::Impl {
public:
    Impl(dmt::Backend backend,
         int device_id,
         prepare::rfi::RfiConfig cfg,
         const prepare::rfi::BlockLayout& layout,
         float baseline_length);

    RfiDeviceOutput execute(std::span<const uint8_t> host,
                            SizeType stride,
                            SizeType nsamps,
                            SizeType block,
                            dmt::Stream stream);

    void download(std::span<uint8_t> host,
                  SizeType stride,
                  SizeType nsamps,
                  dmt::Stream stream) const;

    dmt::DeviceSpan<const float> normalize_to_fdmt(SizeType nchans,
                                                   SizeType nsamps,
                                                   SizeType valid,
                                                   bool flip,
                                                   dmt::Stream stream);

    [[nodiscard]] SizeType memory_usage() const noexcept;

private:
    template <typename T>
    void copy_d2h(std::vector<T>& host, const T* dev, int n, cudaStream_t q) {
        host.resize(static_cast<std::size_t>(n));
        if (n <= 0) {
            return;
        }
        FROST_CUDA_CHECK(cudaMemcpyAsync(
            host.data(), dev, sizeof(T) * static_cast<std::size_t>(n),
            cudaMemcpyDeviceToHost, q));
    }

    void upload_mask(cudaStream_t q);

    // One full clean, run in four phases: measurement
    // (device_rfi_measure_cuda.cu), flag combination and apply
    // (device_rfi_cuda.cu), matching docs/rfi.md.
    template <DataType D>
    void clean(cudaStream_t q,
               int nsamps,
               int block,
               int nchans,
               int row,
               prepare::rfi::RfiReport& report);

    // device_rfi_measure_cuda.cu: fills m_loc/m_scale/m_inv/m_unmeasured
    // and sets report.t_bandpass.
    template <DataType D>
    void measure_bandpass(cudaStream_t q,
                          int nsamps,
                          int nchans,
                          int row,
                          prepare::rfi::RfiReport& report);

    // device_rfi_measure_cuda.cu: fills m_sdet_host/m_cmin_host/m_cmax_host,
    // sets report.t_scan, and returns the number of statically-unmasked
    // channels (0 skips the per-sample kernels entirely, as on the host).
    template <DataType D>
    SizeType scan(cudaStream_t q,
                  int nsamps,
                  int nchans,
                  int row,
                  prepare::rfi::RfiReport& report);

    // device_rfi_cuda.cu: host-only dead-channel + broad-band pass,
    // fills m_time_flags from m_sdet_host/m_cmin_host/m_cmax_host.
    void build_flags(int nsamps,
                     int nchans,
                     SizeType static_good,
                     std::vector<uint8_t>& time_flags,
                     prepare::rfi::RfiReport& report);

    // device_rfi_cuda.cu: neighbour-fill of flagged cells/rows.
    template <DataType D>
    void apply(cudaStream_t q,
               int nsamps,
               int nchans,
               int row,
               const std::vector<uint8_t>& time_flags,
               prepare::rfi::RfiReport& report);

    void validate(float baseline_length) const;
    void build_static_mask();

    int m_device_id = 0;
    prepare::rfi::RfiConfig m_cfg;
    prepare::rfi::BlockLayout m_layout;
    SizeType m_window_len          = 0;
    SizeType m_smooth_radius       = 0;
    SizeType m_time_radius         = 0;
    SizeType m_chan_radius         = 0;
    SizeType m_row_bytes           = 0;
    bool m_narrow                  = false;
    float m_clip                   = 0.0F;
    uint64_t m_block_index         = 0;
    bool m_have_block              = false;
    SizeType m_block               = 0;
    SizeType m_nsamps              = 0;
    SizeType m_calibration_windows = 0;
    std::vector<uint8_t> m_static;
    std::vector<uint8_t> m_mask_host;

    // Host copies of the scan phase's device outputs, consumed by
    // build_flags() immediately afterwards (not retained across blocks).
    std::vector<float> m_sdet_host;
    std::vector<float> m_cmin_host;
    std::vector<float> m_cmax_host;
    std::vector<int> m_unmeasured_host;

    mutable gpu::DeviceVector<uint8_t> m_bytes;
    gpu::DeviceVector<uint8_t> m_orig; // pre-fill snapshot for apply_unflagged
    gpu::DeviceVector<float> m_normalized;
    gpu::DeviceVector<float> m_samples;
    gpu::DeviceVector<float> m_work;
    gpu::DeviceVector<float> m_work2;
    gpu::DeviceVector<float> m_loc;
    gpu::DeviceVector<float> m_scale;
    gpu::DeviceVector<float> m_inv;
    gpu::DeviceVector<int> m_unmeasured;
    gpu::DeviceVector<float> m_sdet;
    gpu::DeviceVector<float> m_cmin;
    gpu::DeviceVector<float> m_cmax;
    gpu::DeviceVector<uint8_t> m_mask;
    gpu::DeviceVector<uint8_t> m_time_flags;
    gpu::DeviceVector<int> m_counters;
};

#define FROST_RFI_EXTERN_MEASURE_BANDPASS(d)                                   \
    extern template void DeviceRfi::Impl::measure_bandpass<d>(                 \
        cudaStream_t, int, int, int, prepare::rfi::RfiReport&);
FROST_RFI_FOR_EACH_DEVICE_CODEC(FROST_RFI_EXTERN_MEASURE_BANDPASS)
#undef FROST_RFI_EXTERN_MEASURE_BANDPASS

#define FROST_RFI_EXTERN_SCAN(d)                                               \
    extern template SizeType DeviceRfi::Impl::scan<d>(                         \
        cudaStream_t, int, int, int, prepare::rfi::RfiReport&);
FROST_RFI_FOR_EACH_DEVICE_CODEC(FROST_RFI_EXTERN_SCAN)
#undef FROST_RFI_EXTERN_SCAN

#define FROST_RFI_EXTERN_APPLY(d)                                              \
    extern template void DeviceRfi::Impl::apply<d>(                            \
        cudaStream_t, int, int, int, const std::vector<uint8_t>&,              \
        prepare::rfi::RfiReport&);
FROST_RFI_FOR_EACH_DEVICE_CODEC(FROST_RFI_EXTERN_APPLY)
#undef FROST_RFI_EXTERN_APPLY

} // namespace frost::pipeline::device
