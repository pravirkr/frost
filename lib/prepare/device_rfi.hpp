#pragma once

// Host-facing device RFI. This header does not include a CUDA or HIP
// header, so lib/pipeline/pipeline.cpp can include it in a CPU build. The
// kernels live in lib/cuda/device_rfi_measure_cuda.cu (bandpass, scan,
// normalization) and lib/cuda/device_rfi_cuda.cu (core, flags, apply),
// and are linked only when FROST_GPU is CUDA or HIP.

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <dmt/common/backend.hpp>

#include <frost/common/policies.hpp>
#include <frost/prepare/rfi.hpp>

namespace frost::pipeline::device {

SizeType available_memory(dmt::Backend backend, int device_id);

/**
 * Absolute margin for a future device comparison of `fill=noise`,
 * zero-DM, zdot and equalise. Those passes are not bit-exact under
 * `-ffast-math` / `-use_fast_math` and stay on `RFICleaner`.
 */
inline constexpr float kFloatFillAbsTol = 1.0e-3F;

/** Host mask and report from one device clean. */
struct RfiDeviceOutput {
    std::vector<uint8_t> channel_mask;
    prepare::rfi::RfiReport report;
};

/**
 * Default integer neighbour excision on device: narrow + broad +
 * neighbour fill, bit-exact with `RFICleaner` (same counter hash, not
 * cuRAND). IQRM and spectral moments are not this path; they would
 * download per-channel stats and finish on the host. `fill=noise`,
 * zero-DM, zdot and equalise are tolerance-only (`kFloatFillAbsTol`)
 * and also stay on the host cleaner.
 *
 * Broadband flags are the host `BaselineFilterPlan` / MAD /
 * `MatchedFilterPlan` applied to a device `s_det` series, so the flag
 * vector matches the CPU cleaner. The cleaned bytes are written on
 * device. Unflagged samples are not rewritten.
 */
class DeviceRfi {
public:
    DeviceRfi(dmt::Backend backend,
              int device_id,
              prepare::rfi::RfiConfig cfg,
              const prepare::rfi::BlockLayout& layout,
              float baseline_length);
    DeviceRfi(const DeviceRfi&)            = delete;
    DeviceRfi& operator=(const DeviceRfi&) = delete;
    DeviceRfi(DeviceRfi&&) noexcept;
    DeviceRfi& operator=(DeviceRfi&&) noexcept;
    ~DeviceRfi();

    /**
     * True for the default RFI configuration on an integer dtype: narrow and
     * broad on, neighbour fill, no moments, IQRM, dropout, Kadane, mask
     * growth, zero-DM or equalise. Static zaps are allowed. 4-bit and
     * narrower data still qualifies; narrow excision is inactive there,
     * as on the host.
     */
    [[nodiscard]] SizeType memory_usage() const noexcept;
    static bool default_integer_neighbour(const prepare::rfi::RfiConfig& cfg,
                                          DataType dtype);

    /**
     * Clean the first `nsamps` tightly packed rows of `host` (`stride`
     * must equal the row size). The device buffer is `block` rows; the
     * tail is zero. Synchronises `stream` before returning. `stream`
     * must be the pipeline's explicit stream, not the null stream.
     */
    RfiDeviceOutput execute(std::span<const uint8_t> host,
                            SizeType stride,
                            SizeType nsamps,
                            SizeType block,
                            dmt::Stream stream);

    /** Copy the cleaned rows back. `host` is `nsamps * stride` bytes. */
    void download(std::span<uint8_t> host,
                  SizeType stride,
                  SizeType nsamps,
                  dmt::Stream stream) const;

    /** Calibrated channel-major float waterfall, with masked channels and
     * samples past valid_nsamps zeroed. The returned device view is reused. */
    dmt::DeviceSpan<const float> normalize_to_fdmt(SizeType nchans,
                                                   SizeType nsamps,
                                                   SizeType valid_nsamps,
                                                   bool flip_chans,
                                                   dmt::Stream stream);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

inline bool
DeviceRfi::default_integer_neighbour(const prepare::rfi::RfiConfig& cfg,
                                     DataType dtype) {
    if (is_floating(dtype)) {
        return false;
    }
    if (!cfg.narrow.enabled || !cfg.broad.enabled) {
        return false;
    }
    if (cfg.fill != prepare::rfi::FillMode::kNeighbour) {
        return false;
    }
    if (cfg.zero_dm != prepare::rfi::ZeroDMMode::kOff || cfg.equalize) {
        return false;
    }
    if (cfg.moments.enabled || cfg.iqrm.enabled || cfg.dropout.enabled) {
        return false;
    }
    if (cfg.kadane_channel.enabled || cfg.kadane_spectrum.enabled) {
        return false;
    }
    return cfg.mask_grow == 0;
}

/** Owns the pipeline's device queue and the upload buffer for host RFI. */
class WaterfallBuffer {
public:
    WaterfallBuffer(dmt::Backend backend, int device_id, SizeType capacity);
    ~WaterfallBuffer();
    WaterfallBuffer(WaterfallBuffer&&) noexcept;
    WaterfallBuffer& operator=(WaterfallBuffer&&) noexcept;
    WaterfallBuffer(const WaterfallBuffer&)            = delete;
    WaterfallBuffer& operator=(const WaterfallBuffer&) = delete;
    dmt::Stream stream() const;
    [[nodiscard]] SizeType memory_usage() const noexcept;
    dmt::DeviceSpan<const float> upload(std::span<const float> waterfall);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

/**
 * Host copy of the CPU broad-band detector, compiled with the host
 * fast-math flags so a `boxcar_max > 1` widening matches `RFICleaner`.
 */
SizeType broadband_time_flags(std::vector<float> series,
                              SizeType smooth_radius,
                              float threshold,
                              SizeType boxcar_max,
                              std::span<uint8_t> flags);

} // namespace frost::pipeline::device
