#pragma once

#include <optional>

#include <frost/common/policies.hpp>

namespace frost::search {

/** Location of a detection in its own frequency plane, before association. */
struct CandidateLocation {
    SizeType samp_idx     = 0;
    float dm              = 0;
    SizeType filter_width = 0;
    float f_low           = 0;
    float f_high          = 0;
};

/**
 * A single detected single-pulse candidate ("giant"), the peak of one
 * above-threshold segment of one boxcar-filtered, one-DM-trial time
 * series. One shared definition reused by giant-finding, clustering and
 * merging (and, later, any multi-beam coincidencer), so every stage
 * applies the same coincidence predicate.
 */
struct Giant {
    float snr         = 0.0F;
    float dm          = 0.0F;
    SizeType samp_idx = 0; // sample index of the peak
    SizeType filter_idx =
        0; // width-trial index; log2(width) when widths double
    SizeType filter_width =
        0; // boxcar width in samples; 0 derives 1<<filter_idx
    SizeType dm_idx  = 0;
    SizeType begin   = 0; // first above-threshold sample in the segment
    SizeType end     = 0; // last above-threshold sample in the segment
    SizeType members = 1; // samples merged into this candidate so far
    // Frequency extent of the dedispersed band this giant came from.
    // Within one plane, clustering requires this extent to match.
    // Nested extents of one pulse are collapsed later, along the
    // dispersion curve, to the wider channel span.
    float f_low         = 0.0F; // MHz, bottom edge
    float f_high        = 0.0F; // MHz, top edge
    SizeType chan_begin = 0;    // inclusive original channel index
    SizeType chan_end   = 0;    // inclusive original channel index
    // FDMT view (plane) of the gulp this giant came from. Equal extents
    // from different planes are copies of one node searched twice.
    SizeType plane = 0;
    // Strongest member of a sub-band dedup merge and the extent it was
    // found in. `snr_best` is 0 until set; `best_snr()` falls back to `snr`.
    float snr_best           = 0.0F;
    SizeType best_chan_begin = 0;
    SizeType best_chan_end   = 0;
    SizeType n_extents       = 1; // distinct channel extents merged here
    // Preserves the coordinates paired with snr_best, independently of the
    // widest-band representative. Empty for a row not yet deduplicated.
    std::optional<CandidateLocation> location_best;

    [[nodiscard]] float best_snr() const noexcept {
        return snr_best > snr ? snr_best : snr;
    }
    [[nodiscard]] bool has_best_extent() const noexcept {
        return location_best.has_value() || snr_best > snr;
    }
    [[nodiscard]] CandidateLocation strongest_location() const noexcept;
};

/**
 * Boxcar width in samples: `filter_width`, or `1 << filter_idx` for
 * hand-built giants. The shift is bounded so a corrupt index cannot be
 * undefined behavior; it saturates at the top bit.
 */
[[nodiscard]] constexpr SizeType giant_boxcar_width(const Giant& g) noexcept {
    if (g.filter_width > 0) {
        return g.filter_width;
    }
    constexpr SizeType kTopBit = (sizeof(SizeType) * 8) - 1;
    return SizeType{1} << (g.filter_idx < kTopBit ? g.filter_idx : kTopBit);
}

inline CandidateLocation Giant::strongest_location() const noexcept {
    return location_best.value_or(CandidateLocation{
        samp_idx, dm, giant_boxcar_width(*this), f_low, f_high});
}

/**
 * Coincidence predicate shared by clustering and any future
 * multi-beam coincidencer: two giants are considered the same event if
 * they fall within `time_tol` (scaled by the wider boxcar width), `filter_tol`,
 * and `dm_tol` of each other, and they share both a plane and channel extent.
 * Different extents stay apart here: `dm_idx` is only meaningful
 * inside one band's trial list. Nested sub-band rows are a later pass
 * (`SubbandDedupPlan`), not a weakening of this predicate.
 */
bool are_coincident(const Giant& a,
                    const Giant& b,
                    SizeType time_tol,
                    SizeType filter_tol,
                    SizeType dm_tol) noexcept;

struct ClusterTolerances;

/**
 * As above, with the physical DM gate of `tol` (see `ClusterTolerances`):
 * beyond adjacent trials, the DM difference must lie within `dm_smear`
 * boxcar widths of dispersive smearing across the giants' band.
 */
bool are_coincident(const Giant& a,
                    const Giant& b,
                    const ClusterTolerances& tol) noexcept;

} // namespace frost::search
