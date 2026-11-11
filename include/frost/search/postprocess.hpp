#pragma once
#include <memory>
#include <span>

#include <dmt/common/candidate.hpp>

#include <frost/config/config.hpp>
#include <frost/search/candidate.hpp>

namespace frost::search {

struct CandidatePostprocessLayout {
    SizeType nchans       = 0;
    double tsamp          = 0.0;
    double frequency_step = 0.0;
};

/** Plan-owned output, valid until the next execute(). All members of recent
 * clusters remain pending until later peaks close, or the final call. */
struct CandidatePostprocessResult {
    std::span<const Giant> candidates;
    double cluster_s            = 0.0;
    double dedup_s              = 0.0;
    SizeType pending_candidates = 0; // Retained members, not reduced winners.
};

/** Consumes an FDMTBoxcar candidate view immediately, converts coordinates,
 * rate-limits, clusters across gulps and deduplicates sub-band extents. */
class CandidatePostprocessPlan {
public:
    CandidatePostprocessPlan(const config::SearchConfig& cfg,
                             CandidatePostprocessLayout layout);
    ~CandidatePostprocessPlan();
    CandidatePostprocessPlan(CandidatePostprocessPlan&&) noexcept;
    CandidatePostprocessPlan& operator=(CandidatePostprocessPlan&&) noexcept;
    CandidatePostprocessPlan(const CandidatePostprocessPlan&) = delete;
    CandidatePostprocessPlan&
    operator=(const CandidatePostprocessPlan&) = delete;

    /** samples_read is the real input end; duration_samples is used by the
     * per-plane rate limiter. final drains all pending rows at EOF.
     * candidate_frontier is FDMTBoxcar::get_candidate_frontier() after this
     * result: no later raw peak precedes it, so groups whose members all
     * end more than (cand_sep_time + 1) * boxcar_max samples before it are
     * released, however long an excursion stays open. It is ignored when
     * final. The dmt view is never retained beyond this call. Input contains
     * only beam 0, positive widths no larger than boxcar_max, and inclusive
     * excursions enclosing their peaks. Invalid coordinates throw. */
    CandidatePostprocessResult execute(dmt::CandidateResultView result,
                                       SizeType samples_read,
                                       SizeType duration_samples,
                                       bool final,
                                       SizeType candidate_frontier);
    /** Reserved persistent candidate bytes (excludes bounded algorithm
     * scratch). */
    [[nodiscard]] SizeType memory_usage() const noexcept;
    /** Conservative persistent+temporary bound; does not allocate. */
    static SizeType estimate_memory(const config::SearchConfig& cfg);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace frost::search
