#pragma once

// Private implementation detail of RFICleaner, split by phase (see
// docs/rfi.md) across lib/cpu/rfi_cpu_measure.cpp (bandpass/moments
// measurement and the per-row scan feeding the time- and cell-level
// detectors) and lib/cpu/rfi_cpu.cpp (orchestration, flag combination
// and the final write-back pass). Each .cpp defines a subset of
// RFICleaner::Impl's member functions; this header is the single
// declaration all of them implement against. Not a public header: never
// included outside lib/cpu/.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

#include <frost/common/policies.hpp>
#include <frost/prepare/rfi.hpp>

#include "lib/cpu/rfi_kernels.hpp"

namespace frost::prepare::rfi {

namespace detail {

using Clock = std::chrono::steady_clock;

constexpr SizeType kMaxBandpassRows = 3125; // 5^5
constexpr SizeType kMinWindow       = 5;    // measure_bandpass minimum
// Below this many spectra a per-channel median and MAD are too noisy to flag
// anything: a channel that happens to be constant across a short tail would be
// declared dead and killed for that gulp.
constexpr SizeType kMinBlockSamps = 64;
constexpr int kMaxDraws           = 10; // neighbour-fill resample attempts
constexpr SizeType kChanBlock     = 64;
// The row scan is split into a fixed number of chunks that depends only
// on nsamps, so per-chunk partial sums combine identically whatever the
// thread count.
constexpr SizeType kMaxChunks    = 64;
constexpr SizeType kMinChunkRows = 1024;

// Random-stream tags, so draws for different purposes never collide.
enum Stream : uint64_t {
    kStreamCell   = 1,
    kStreamTime   = 2,
    kStreamNoise  = 3,
    kStreamMasked = 4,
};

enum CellState : uint8_t {
    kClean,
    kMasked,
    kUnmeasured, // no rms in this window: left as is, never a fill source
    kNarrow,
    kKadaneSpectrum,
    kKadaneChannel,
};

struct MomentSums {
    double n   = 0.0;
    double s1  = 0.0;
    double s2  = 0.0;
    double s3  = 0.0;
    double s4  = 0.0;
    double lag = 0.0; // sum of z_t * z_{t-1}
    double nc  = 0.0; // clipped (|z| <= clip) sums
    double c1  = 0.0;
    double c2  = 0.0;
};

struct ZdotSums {
    double n   = 0.0;
    double sz  = 0.0;
    double ss  = 0.0;
    double sss = 0.0;
    double szs = 0.0;
};

struct Span {
    int32_t lo = -1;
    int32_t hi = -1;

    bool contains(SizeType c) const {
        return lo >= 0 && static_cast<int32_t>(c) >= lo &&
               static_cast<int32_t>(c) <= hi;
    }
};

struct Segment {
    IndexType start = 0;
    IndexType end   = -1;

    bool contains(SizeType t) const {
        const auto i = static_cast<IndexType>(t);
        return i >= start && i <= end;
    }
};

inline SizeType count_zeros(std::span<const uint8_t> v) {
    return static_cast<SizeType>(std::count(v.begin(), v.end(), uint8_t{0}));
}

} // namespace detail

/** Calls `F(C)` for every sample-depth codec RFICleaner::Impl is
 * instantiated against, so each split .cpp can explicitly instantiate or
 * extern-declare its templated members once for all of them. */
#define FROST_RFI_FOR_EACH_CODEC(F)                                            \
    F(kernels::Codec<DataType::kUInt1>)                                        \
    F(kernels::Codec<DataType::kUInt2>)                                        \
    F(kernels::Codec<DataType::kUInt4>)                                        \
    F(kernels::Codec<DataType::kUInt8>)                                        \
    F(kernels::Codec<DataType::kUInt16>)                                       \
    F(kernels::Codec<DataType::kUInt32>)                                       \
    F(kernels::Codec<DataType::kFloat32>)

class RFICleaner::Impl {
public:
    Impl(RfiConfig cfg, const BlockLayout& layout, float baseline_length);

    void execute(std::span<uint8_t> block, bool validated = false);
    std::span<const float> execute_normalized(std::span<uint8_t> block,
                                              SizeType valid);

    SizeType memory_usage() const noexcept;

    std::span<const uint8_t> channel_mask() const { return m_mask; }
    const RfiReport& report() const { return m_report; }
    const RfiConfig& config() const { return m_cfg; }

private:
    RfiConfig m_cfg;
    BlockLayout m_layout;
    SizeType m_nchans;
    SizeType m_row_bytes;
    SizeType m_window_len       = 0;
    SizeType m_smooth_radius    = 0;
    SizeType m_time_fill_radius = 0;
    SizeType m_chan_fill_radius = 0;
    float m_clip                = 0.0F;
    bool m_narrow_active        = false;
    bool m_transform            = false;
    bool m_need_moments         = false;
    float m_eq_loc              = 0.0F;
    float m_eq_scale            = 1.0F;
    uint64_t m_block_index      = 0;

    std::vector<uint8_t> m_static_mask;
    std::vector<uint8_t> m_mask;
    RfiReport m_report;

    std::vector<float> m_work;
    std::vector<float> m_normalized;
    // Last reliable window, before and after cleaning/transforms. Short
    // normalized tails reuse it without fitting their own signal away.
    bool m_tail_ready = false;
    std::vector<float> m_tail_loc, m_tail_scale;
    std::vector<float> m_tail_output_loc, m_tail_output_scale;
    void normalize_short(SizeType capacity, SizeType valid);

    // Current block.
    uint8_t* m_data   = nullptr;
    SizeType m_nsamps = 0;
    SizeType m_nwin   = 0;

    // Per window x channel.
    std::vector<float> m_loc;
    std::vector<float> m_scale;
    std::vector<float> m_inv_scale;
    std::vector<detail::MomentSums> m_moments;
    std::vector<float> m_clip_mean;
    std::vector<float> m_clip_inv_std;
    std::vector<float> m_bandpass_rows;
    std::vector<SizeType> m_unmeasured; // windows with no rms, per channel

    // Per time sample.
    std::vector<float> m_s_det;  // clipped band mean: detection series
    std::vector<float> m_s_keep; // band mean of non-outlier cells
    std::vector<float> m_row_std;
    std::vector<detail::Span> m_kspec_pos;
    std::vector<detail::Span> m_kspec_neg;
    std::vector<uint8_t> m_kspec_whole;
    std::vector<uint8_t> m_time_flags;
    std::vector<SizeType> m_good_prefix; // good channels before c

    // Per channel.
    std::vector<float> m_zdot_a;
    std::vector<float> m_zdot_b;
    std::vector<detail::Segment> m_kchan_pos;
    std::vector<detail::Segment> m_kchan_neg;

    // Per chunk x channel.
    std::vector<detail::ZdotSums> m_zdot_sums;
    std::vector<kernels::KadaneState> m_kchan_state_pos;
    std::vector<kernels::KadaneState> m_kchan_state_neg;
    std::vector<float> m_chunk_min; // over unmeasured cells only
    std::vector<float> m_chunk_max;

    void validate(float baseline_length) const;
    void build_static_mask();

    // Small, perf-sensitive accessors shared by every phase (bandpass,
    // moments, scan and apply each live in their own .cpp): kept inline
    // here so they can still be inlined across those translation units.
    SizeType window_of(SizeType t) const {
        return std::min(t / m_window_len, m_nwin - 1);
    }
    SizeType window_begin(SizeType w) const { return w * m_window_len; }
    SizeType window_end(SizeType w) const {
        return w + 1 == m_nwin ? m_nsamps : (w + 1) * m_window_len;
    }
    const uint8_t* row(SizeType t) const { return m_data + (t * m_row_bytes); }
    uint8_t* row(SizeType t) { return m_data + (t * m_row_bytes); }
    SizeType chunk_count() const {
        const SizeType by_rows =
            (m_nsamps + detail::kMinChunkRows - 1) / detail::kMinChunkRows;
        return std::clamp<SizeType>(by_rows, 1, detail::kMaxChunks);
    }

    // Robust per-window bandpass level and rms (lib/cpu/rfi_cpu_measure.cpp).
    template <typename C> void measure_bandpass();

    // Per window/channel raw and clipped moments, and the dynamic channel
    // mask (moments outliers, IQRM, mask growth) built from them
    // (lib/cpu/rfi_cpu_measure.cpp).
    template <typename C> void measure_moments();
    void build_channel_mask();
    void flag_moment_outliers(std::span<uint8_t> flags) const;

    // One pass over every row feeding the time- and cell-level detectors:
    // zero-DM series, spectral spread, Kadane along frequency and time,
    // zdot (lib/cpu/rfi_cpu_measure.cpp).
    template <typename C> void scan_rows();

    // Combines the scan's per-row and per-channel statistics into the
    // final time-sample and channel flags (lib/cpu/rfi_cpu.cpp).
    void build_flags();

    // Cleans the block in place: fills flagged cells/rows, and applies
    // any zero-DM/equalisation transform (lib/cpu/rfi_cpu.cpp).
    float zero_dm_term(SizeType c, SizeType t) const;
    uint64_t
    draw(detail::Stream stream, SizeType t, SizeType c, int attempt) const;
    float level(SizeType w, SizeType c, float v) const;
    template <typename C>
    float denormalise(SizeType w, SizeType c, float v) const;
    float to_noise_units(SizeType w, SizeType c, float out) const;
    template <typename C>
    float
    fill_value(detail::Stream stream, SizeType w, SizeType t, SizeType c) const;
    template <typename C> void apply();
};

// Instantiated once, in the .cpp that defines each template member; every
// other .cpp only ever sees these declarations, so execute()'s dispatch
// in rfi_cpu.cpp links against them instead of re-instantiating.
#define FROST_RFI_EXTERN_IMPL_METHODS(C)                                       \
    extern template void RFICleaner::Impl::measure_bandpass<C>();              \
    extern template void RFICleaner::Impl::measure_moments<C>();               \
    extern template void RFICleaner::Impl::scan_rows<C>();                     \
    extern template void RFICleaner::Impl::apply<C>();
FROST_RFI_FOR_EACH_CODEC(FROST_RFI_EXTERN_IMPL_METHODS)
#undef FROST_RFI_EXTERN_IMPL_METHODS

} // namespace frost::prepare::rfi
