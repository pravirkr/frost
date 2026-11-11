#include <algorithm>
#include <array>
#include <limits>
#include <numeric>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <frost/search/clustering.hpp>

namespace {
using frost::SizeType;
using frost::search::ClusterTolerances;
using frost::search::Giant;

// Compare against a mathematical product without multiplying: a time
// separation fits if the ceiling of separation/width fits the tolerance.
bool reference_coincident(const Giant& a,
                          const Giant& b,
                          const ClusterTolerances& tol) {
    const auto distance = [](SizeType x, SizeType y) {
        return x > y ? x - y : y - x;
    };
    const auto width = std::max(a.filter_width, b.filter_width);
    const auto dt    = distance(a.samp_idx, b.samp_idx);
    return a.plane == b.plane && a.chan_begin == b.chan_begin &&
           a.chan_end == b.chan_end &&
           dt / width + (dt % width != 0) <= tol.time_tol &&
           distance(a.filter_idx, b.filter_idx) <= tol.filter_tol &&
           distance(a.dm_idx, b.dm_idx) <= tol.dm_tol;
}

std::vector<SizeType> reference_labels(const std::vector<Giant>& candidates,
                                       const ClusterTolerances& tol,
                                       frost::search::ClusterMode mode) {
    std::vector<SizeType> labels(candidates.size());
    std::iota(labels.begin(), labels.end(), SizeType{0});
    if (mode == frost::search::ClusterMode::kChain) {
        const auto root = [&labels](SizeType x) {
            while (labels[x] != x)
                x = labels[x];
            return x;
        };
        for (SizeType i = 0; i < candidates.size(); ++i) {
            for (SizeType j = i + 1; j < candidates.size(); ++j) {
                if (reference_coincident(candidates[i], candidates[j], tol)) {
                    const auto a = root(i), b = root(j);
                    labels[std::max(a, b)] = std::min(a, b);
                }
            }
        }
        for (SizeType i = 0; i < labels.size(); ++i)
            labels[i] = root(i);
    } else {
        auto order = labels;
        std::stable_sort(order.begin(), order.end(),
                         [&](SizeType a, SizeType b) {
                             return candidates[a].snr > candidates[b].snr;
                         });
        std::vector<bool> assigned(candidates.size());
        for (const auto seed : order) {
            if (assigned[seed])
                continue;
            assigned[seed] = true;
            for (SizeType j = 0; j < candidates.size(); ++j) {
                if (!assigned[j] && reference_coincident(candidates[seed],
                                                         candidates[j], tol)) {
                    assigned[j] = true;
                    labels[j]   = seed;
                }
            }
        }
    }
    return labels;
}
} // namespace

TEST_CASE(
    "clustering respects mathematical time tolerance at integer boundaries",
    "[clustering][contracts][overflow]") {
    constexpr auto kMax = std::numeric_limits<SizeType>::max();
    std::vector<Giant> candidates;
    for (SizeType width :
         {SizeType{1}, SizeType{2}, SizeType{3}, kMax / 2 + 1, kMax}) {
        for (SizeType sample : {SizeType{0}, SizeType{1}, SizeType{63},
                                kMax / 2, kMax - 1, kMax}) {
            Giant candidate;
            candidate.samp_idx     = sample;
            candidate.filter_width = width;
            candidate.snr = static_cast<float>(candidates.size() % 7 + 1);
            candidates.push_back(candidate);
        }
    }
    for (const auto time : {SizeType{0}, SizeType{1}, SizeType{2}, kMax / 2,
                            kMax / 2 + 1, kMax}) {
        const ClusterTolerances tol{time, 0, 0};
        for (const auto& a : candidates) {
            for (const auto& b : candidates) {
                CHECK(frost::search::are_coincident(a, b, tol) ==
                      reference_coincident(a, b, tol));
            }
        }
        for (const auto mode : {frost::search::ClusterMode::kChain,
                                frost::search::ClusterMode::kSeeded}) {
            CHECK(frost::search::ClusterLabelPlan(mode).execute(candidates,
                                                                tol) ==
                  reference_labels(candidates, tol, mode));
        }
    }
}
