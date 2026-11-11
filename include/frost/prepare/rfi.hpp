#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include <frost/common/policies.hpp>

namespace frost::prepare::rfi {

/** A single inclusive [start, end] channel index range. */
struct ChannelRange {
    SizeType start;
    SizeType end;
    bool operator==(const ChannelRange&) const = default;
};

/** An inclusive [lo, hi] frequency range in MHz. */
struct FrequencyRange {
    double lo;
    double hi;
    bool operator==(const FrequencyRange&) const = default;
};

/**
 * Reads Frost's channel kill file: the sigproc convention of one `1` (keep)
 * or `0` (kill) per line, one line per channel, in the file's stored channel
 * order. Blank lines and `#` comments (whole-line or trailing) are ignored.
 * Parsing is strict so that a file for another channel count or format fails
 * rather than masking the wrong channels: it throws std::invalid_argument
 * unless every remaining line is exactly `0` or `1` and there are exactly
 * `nchans` of them, and std::runtime_error if the file cannot be read.
 * Returns the killed channels as merged inclusive ranges, in stored order.
 */
std::vector<ChannelRange> read_kill_file(const std::filesystem::path& path,
                                         SizeType nchans);

/**
 * Zeros out `killmask[c]` for every channel in each range of
 * `channel_zaps` (channels already zero stay zero). `killmask` has one
 * entry per channel (1 = keep, 0 = killed).
 */
void apply_manual_killmask(std::span<uint8_t> killmask,
                           std::span<const ChannelRange> channel_zaps);

/**
 * IQRM (Morello et al. 2022, MNRAS 511, 1116): flags channels whose
 * per-channel statistic `x` (larger = more contaminated, e.g. the
 * standard deviation) is a high outlier relative to its neighbours at a
 * geometric series of channel lags up to `radius`, via a Tukey-rule vote.
 * Channels with `ignore[c] != 0` take no part and are returned flagged;
 * `ignore` may be empty. Returns 1 for each flagged channel.
 */
std::vector<uint8_t> iqrm_mask(std::span<const float> x,
                               SizeType radius,
                               float threshold,
                               std::span<const uint8_t> ignore = {});

/** How flagged samples are replaced. */
enum class FillMode : uint8_t {
    // A flagged cell takes the (rescaled) value of a random clean
    // channel within +-nchans/60 at the same time; a flagged time sample
    // takes, per channel, the value at a random unflagged time within
    // +-baseline/8. Falls back to kMean after 10 failed draws.
    kNeighbour,
    kMean,  // the channel's local bandpass level
    kNoise, // Gaussian noise at the channel's local level and rms
};

/** Zero-DM filtering, applied to every sample kept after flagging. */
enum class ZeroDMMode : uint8_t {
    kOff,
    // Eatough et al. 2009: subtract the per-sample
    // band mean (in each channel's own noise units).
    kSubtract,
    // Men et al. 2019 "zdot": subtract a per-channel least-squares fit
    // a_c * s(t) + b_c of the band-mean series, so channels with a
    // different response to broadband RFI are cleaned properly.
    kZdot,
};

/** Narrow-band excision: cells more than `threshold` robust sigma from
 * their channel's local bandpass. Skipped for <= 4-bit data. `threshold` is
 * also the outlier clip used by the zero-DM statistics and the Kadane searches.
 */
struct NarrowbandConfig {
    bool enabled                                   = true;
    float threshold                                = 5.0F;
    bool operator==(const NarrowbandConfig&) const = default;
};

/** Broad-band excision: time samples whose zero-DM (band-mean)
 * series, baseline-subtracted and MAD-normalised, exceeds `threshold` at
 * any boxcar width 1, 2, 4, ..., `boxcar_max`. */
struct BroadbandConfig {
    bool enabled                                  = true;
    float threshold                               = 5.0F;
    SizeType boxcar_max                           = 1;
    bool operator==(const BroadbandConfig&) const = default;
};

/** Dropped/blanked time samples:
 * spectra whose spread across channels is below `min_std_fraction` of the
 * block's median spread. The samples either side are flagged too. */
struct DropoutConfig {
    bool enabled                                = false;
    float min_std_fraction                      = 0.1F;
    bool operator==(const DropoutConfig&) const = default;
};

/** Per-channel spectral-moment outliers: a channel is masked if any selected
 * statistic of its robustly-normalised samples lies more than `threshold`
 * interquartile ranges outside [Q1, Q3] across channels, or its
 * |excess kurtosis| exceeds `kurtosis_max` (0 disables that test). */
struct MomentsConfig {
    bool enabled                                = false;
    float threshold                             = 4.0F;
    bool mean                                   = true;
    bool std                                    = true;
    bool skewness                               = true;
    bool kurtosis                               = true;
    bool autocorr                               = true; // lag-1 autocorrelation
    float kurtosis_max                          = 0.0F;
    bool operator==(const MomentsConfig&) const = default;
};

/** IQRM on the per-channel standard deviation (data units). */
struct IqrmConfig {
    bool enabled    = false;
    float threshold = 3.0F;
    SizeType radius = 0; // 0: nchans / 10, the reference recommendation
    bool operator==(const IqrmConfig&) const = default;
};

/** Kadane search along time (after Men & Barr 2024): per channel, the
 * maximum-sum run of samples along
 * time (positive and negative); flagged if its S/N exceeds `threshold`
 * and it lasts at least `min_duration` seconds. Finds weak, long-lived
 * narrow-band RFI that per-sample thresholds miss. */
struct KadaneChannelConfig {
    bool enabled                                      = false;
    float threshold                                   = 7.0F;
    float min_duration                                = 0.05F; // seconds
    bool operator==(const KadaneChannelConfig&) const = default;
};

/** Kadane search across frequency (after Men & Barr 2024): per time
 * sample, the maximum-sum run of channels
 * (positive and negative); flagged if its S/N exceeds `threshold` and it
 * spans at least `min_bandwidth` MHz. A run covering more than
 * `whole_band_fraction` of the band flags the whole time sample. */
struct KadaneSpectrumConfig {
    bool enabled                                       = false;
    float threshold                                    = 7.0F;
    float min_bandwidth                                = 10.0F; // MHz
    float whole_band_fraction                          = 0.8F;
    bool operator==(const KadaneSpectrumConfig&) const = default;
};

/**
 * Every RFI strategy the cleaner can run, each individually switchable.
 * Defaults are narrow + broad excision with neighbour fill; the rest
 * are opt-in. See docs/rfi.md for the algorithms and their tradeoffs.
 */
struct RfiConfig {
    std::vector<ChannelRange> zap_chans;
    std::vector<FrequencyRange> zap_freqs;
    NarrowbandConfig narrow;
    BroadbandConfig broad;
    DropoutConfig dropout;
    MomentsConfig moments;
    IqrmConfig iqrm;
    // Widens every moments/IQRM-masked channel by this many channels either
    // side.
    SizeType mask_grow = 0;
    KadaneChannelConfig kadane_channel;
    KadaneSpectrumConfig kadane_spectrum;
    ZeroDMMode zero_dm = ZeroDMMode::kOff;
    // Rewrite every channel to a common level and rms: 0 and 1 for float
    // data, 2^(nbits-1) and 2^nbits/16 for integer data.
    bool equalize = false;
    FillMode fill = FillMode::kNeighbour;
    uint64_t seed = 0x5EED;

    /** Rejects non-finite or out-of-range settings, including disabled
     * strategies. Observing-layout constraints are checked by the cleaner. */
    void validate() const;
    bool operator==(const RfiConfig&) const = default;
};

/** Shape and metadata of the blocks a cleaner will be given. */
struct BlockLayout {
    SizeType nchans     = 0;
    SizeType max_nsamps = 0;
    DataType dtype      = DataType::kUInt8;
    double tsamp        = 0.0; // seconds
    double f0           = 0.0; // centre frequency of channel 0, MHz
    double df           = 0.0; // channel spacing, MHz (may be negative)
};

/** What the last `RFICleaner::execute` found and how long it took. */
struct RfiReport {
    bool calibration_reused =
        false; // normalized short tail used the last measured window
    SizeType nsamps                = 0;
    SizeType chans_zapped          = 0; // zap_chans / zap_freqs
    SizeType chans_dead            = 0; // zero variance
    SizeType chans_moments         = 0;
    SizeType chans_iqrm            = 0;
    SizeType chans_grown           = 0;
    SizeType chans_masked          = 0; // total
    SizeType samps_broad           = 0;
    SizeType samps_dropout         = 0;
    SizeType samps_kadane          = 0; // whole-band kadane_spectrum hits
    SizeType samps_flagged         = 0; // total
    SizeType cells_narrow          = 0;
    SizeType cells_kadane_spectrum = 0;
    SizeType cells_kadane_channel  = 0;
    SizeType fill_fallbacks = 0;   // neighbour draws that fell back to kMean
    double t_bandpass       = 0.0; // seconds, per stage
    double t_moments        = 0.0;
    double t_scan           = 0.0;
    double t_flags          = 0.0;
    double t_apply          = 0.0;
};

/**
 * The RFI-excision stage: cleans one block of channelised intensity data
 * (row-major, time-then-channel, packed at `BlockLayout::dtype`) in place,
 * before dedispersion.
 *
 * All enabled strategies share one set of statistics, computed in a small
 * fixed number of passes over the packed block: a robust per-window
 * bandpass (sampled), per-channel moments (only if a channel statistic
 * needs them), one row scan feeding every time- and cell-level detector,
 * and one apply pass that replaces what was flagged and applies the
 * zero-DM/equalisation transforms. Unflagged samples are left bit-exact
 * unless a transform is enabled. Results are deterministic for a given
 * seed and independent of the OpenMP thread count.
 */
class RFICleaner {
public:
    /** Validates configuration and observing geometry before allocating
     * workspace. Packed/normalized block sizes and the baseline sample count
     * must be representable; malformed metadata throws invalid_argument. */
    RFICleaner(RfiConfig cfg, const BlockLayout& layout, float baseline_length);
    RFICleaner(const RFICleaner&)            = delete;
    RFICleaner& operator=(const RFICleaner&) = delete;
    RFICleaner(RFICleaner&&) noexcept;
    RFICleaner& operator=(RFICleaner&&) noexcept;
    ~RFICleaner();

    /** Cleans `block` (a whole number of rows, at most `max_nsamps`) in
     * place. Non-finite float32 input throws before statistics are measured. */
    void execute(std::span<uint8_t> block);

    /** Copies `in` to the front of `out`, then cleans that copy. */
    void execute(std::span<const uint8_t> in, std::span<uint8_t> out);

    /** Cleans in float precision and returns calibrated float32 in
     * [channel][time] order, low frequency first, with contiguous time.
     * Each active channel is calibrated to zero-mean unit noise; masked
     * channels are zero. The search excludes them through channel_mask()
     * (no global rescaling). Padding is zero and excluded from statistics.
     * Non-finite float32 input throws before decoding or measuring statistics.
     * nsamps=0 uses all rows; otherwise only nsamps real rows are measured.
     * Tails below 64 rows reuse the last normalized block's final calibration
     * window and channel mask, applying transforms but no new automatic
     * excision. With no preceding calibration they use their own mean/std.
     * The byte-cleaning execute() API invalidates this retained calibration.
     * The returned storage is valid until the next call. Input is unchanged. */
    std::span<const float> execute_normalized(std::span<uint8_t> block,
                                              SizeType nsamps = 0);

    /** Per channel, 1 = good, 0 = masked in the last block (static zaps
     * only before the first). Masked channels are filled with their fill
     * value and should also be excluded from dedispersion. */
    std::span<const uint8_t> channel_mask() const;

    const RfiReport& report() const;
    const RfiConfig& config() const;
    [[nodiscard]] SizeType memory_usage() const noexcept;

    /** Conservative bound for normalized work, statistics and temporary
     * scratch at this layout/thread count; does not allocate buffers.
     * Applies the same configuration/geometry validation as construction. */
    static SizeType estimate_memory(const RfiConfig& cfg,
                                    const BlockLayout& layout,
                                    float baseline_length,
                                    SizeType threads);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace frost::prepare::rfi
