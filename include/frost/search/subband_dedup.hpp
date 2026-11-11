#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <frost/common/policies.hpp>
#include <frost/search/candidate.hpp>
#include <frost/search/clustering.hpp>

namespace frost::search {

/**
 * Samples to add to a detection timestamped at `f_narrow` so it lines
 * up with an arrival at `f_wide`. `dm` is pc cm^-3, frequencies are
 * MHz, `tsamp` is seconds. Positive when the narrow band's bottom edge
 * is the higher frequency (that plane peaks earlier).
 */
double subband_time_shift_samples(float dm,
                                  float f_wide,
                                  float f_narrow,
                                  float tsamp) noexcept;

/** DM (pc cm^-3) that shifts the band `[f_low, f_high]` by one sample.
 * Zero when the band has no dispersive delay. */
double subband_dm_step(float f_low, float f_high, float tsamp) noexcept;

/**
 * How `SubbandDedupPlan` turns matching pairs into groups.
 *
 * `kTransitive` (default): union-find over every matching pair. A quarter
 * reaches the full band through the half it nests in, and a nested row
 * that matches two wide rows of one plane joins them, although pass 1
 * kept those apart. On real data such rows are mostly DM wings of one
 * bright pulse, so this keeps the row count low; a weak distinct event
 * close to a bright one in time and DM can be absorbed.
 *
 * `kSeeded`: rows are placed widest first (then brightest) and each joins
 * the first placed row it matches; groups never merge, so a nested row
 * cannot bridge two wide rows. Cleaner per-event semantics, but DM wings
 * stay as extra rows (1.1-1.7x more sub-band rows on the benchmark files).
 */
enum class SubbandDedupMode : uint8_t { kTransitive, kSeeded };

/**
 * Deduplicates already within-band-merged giants that lie on one
 * dispersion curve. A pair associates only when the channel ranges
 * nest, the narrower giant's time matches the wider giant's after
 * `subband_time_shift_samples` (using the wider DM), the filter indices
 * agree, and the physical DMs agree within the wider boxcar's worth of
 * dispersive delay on the narrower band (at least two samples). The
 * written row is the larger channel span
 * (`chan_end - chan_begin + 1`), with S/N breaking a span tie;
 * `members` is the sum. `dm_tol` is not used: it is a trial index.
 *
 * How matches form groups is `SubbandDedupMode`.
 *
 * This is a separate pass from `ClusterLabelPlan`. Equal extents of one
 * plane stay apart here (that merge already happened inside one FDMT
 * view); equal extents of two planes (one node searched twice) merge when
 * time, filter and physical DM agree.
 */
class SubbandDedupPlan {
public:
    explicit SubbandDedupPlan(
        SubbandDedupMode mode = SubbandDedupMode::kTransitive)
        : m_mode(mode) {}

    /** `tsamp` is the file sampling time in seconds. An empty list is
     * returned unchanged. */
    std::vector<Giant> execute(std::span<const Giant> candidates,
                               const ClusterTolerances& tol,
                               float tsamp) const;

private:
    SubbandDedupMode m_mode;
};

} // namespace frost::search
