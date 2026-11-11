#include <map>
#include <stdexcept>

#include <frost/search/merge.hpp>

namespace frost::search {

std::vector<Giant>
MergeCandidatesPlan::execute(std::span<const Giant> candidates,
                             std::span<const SizeType> labels) const {
    if (candidates.size() != labels.size()) {
        throw std::invalid_argument("candidates/labels size mismatch");
    }

    // Output is ordered by label, so runs are reproducible.
    std::map<SizeType, Giant> best;
    for (SizeType i = 0; i < candidates.size(); ++i) {
        const SizeType label = labels[i];
        auto it              = best.find(label);
        if (it == best.end()) {
            best.emplace(label, candidates[i]);
            continue;
        }
        const auto members_sum = it->second.members + candidates[i].members;
        if (candidates[i].snr > it->second.snr) {
            it->second = candidates[i];
        }
        it->second.members = members_sum;
    }

    std::vector<Giant> merged;
    merged.reserve(best.size());
    for (const auto& [label, giant] : best) {
        merged.push_back(giant);
    }
    return merged;
}

} // namespace frost::search
