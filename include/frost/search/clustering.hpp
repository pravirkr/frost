#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <frost/common/policies.hpp>
#include <frost/search/candidate.hpp>

namespace frost::search {

/**
 * `dm_tol` counts DM trials (`cand_sep_dm`) and is an upper
 * bound only: on a fine trial grid it can span the whole list. The
 * physical gate, active when `dm_smear > 0` and `tsamp > 0`, needs a DM
 * difference of more than one trial to lie within
 * `dm_smear * widest_boxcar_width * dm_step(band) * g`, where `dm_step *
 * width` is the DM at which a pulse of that width has smeared out of its
 * boxcar and `g = max(1, sqrt((snr_max / snr_floor)^2 - 1))` grows with
 * the brighter giant's S/N (a bright pulse leaves detections further out
 * in DM). `dm_smear == 0` uses the trial count only.
 */
struct ClusterTolerances {
    SizeType time_tol;
    SizeType filter_tol;
    SizeType dm_tol;
    double dm_smear = 0.0;
    double tsamp    = 0.0; // seconds
    // Detection threshold. A pulse of S/N `s` stays above it out to
    // `sqrt((s / snr_floor)^2 - 1)` smearing widths, so the physical gate
    // widens for the brighter giant of a pair. 0 disables that scaling.
    double snr_floor = 0.0;
};

/**
 * How coincidence becomes clusters. `kChain` is
 * friends-of-friends: transitive, so a long chain of weak giants can join
 * distant events. `kSeeded` takes giants by descending S/N; each unassigned
 * giant becomes a seed and claims every unassigned giant coincident with
 * *it*, so a cluster is bounded by one tolerance around its brightest
 * member and a strong pulse is never absorbed by a chain.
 */
enum class ClusterMode : uint8_t { kChain, kSeeded };

/**
 * Pairwise friends-of-friends clustering of candidates, implemented as a
 * union-find over `are_coincident`:
 * the connected components of the coincidence graph. Call it
 * on one FDMT view. Nested sub-band extents are `SubbandDedupPlan`.
 */
class ClusterLabelPlan {
public:
    explicit ClusterLabelPlan(ClusterMode mode = ClusterMode::kChain)
        : m_mode(mode) {}

    /** Returns one label per candidate (the index of its cluster's
     * representative); candidates sharing a label are coincident,
     * directly or transitively (`kChain`) or with the cluster's brightest
     * member (`kSeeded`). Cost is the number of giants within the time
     * tolerance of each other, not N^2: the giants are time-sorted and
     * scanned over a window, with output identical to the pairwise form. */
    std::vector<SizeType> execute(std::span<const Giant> candidates,
                                  const ClusterTolerances& tol) const;

private:
    ClusterMode m_mode;
};

} // namespace frost::search
