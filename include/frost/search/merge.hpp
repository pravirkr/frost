#pragma once

#include <span>
#include <vector>

#include <frost/common/policies.hpp>
#include <frost/search/candidate.hpp>

namespace frost::search {

/**
 * Collapses each cluster produced by `ClusterLabelPlan` to a single
 * representative: the highest-SNR candidate in the cluster, with
 * `members` set to the sum of `members` across the cluster.
 */
class MergeCandidatesPlan {
public:
    MergeCandidatesPlan() = default;

    /** `labels` must have one entry per `candidates` entry (as returned
     * by `ClusterLabelPlan::execute`). */
    std::vector<Giant> execute(std::span<const Giant> candidates,
                               std::span<const SizeType> labels) const;
};

} // namespace frost::search
