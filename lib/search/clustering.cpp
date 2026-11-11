#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include <frost/search/clustering.hpp>
#include <frost/search/subband_dedup.hpp>

namespace frost::search {

namespace {
SizeType abs_diff(SizeType a, SizeType b) noexcept {
    return (a > b) ? (a - b) : (b - a);
}

// A mathematical tolerance wider than the sample-index domain accepts every
// representable distance. Wrapping the product would instead narrow it.
SizeType time_reach(SizeType width, SizeType time_tol) noexcept {
    constexpr auto kMax = std::numeric_limits<SizeType>::max();
    return time_tol != 0 && width > kMax / time_tol ? kMax : width * time_tol;
}
} // namespace

bool are_coincident(const Giant& a,
                    const Giant& b,
                    SizeType time_tol,
                    SizeType filter_tol,
                    SizeType dm_tol) noexcept {
    const auto width_of = [](const Giant& g) { return giant_boxcar_width(g); };
    if (a.plane != b.plane || a.chan_begin != b.chan_begin ||
        a.chan_end != b.chan_end) {
        return false;
    }
    const SizeType widest          = std::max(width_of(a), width_of(b));
    const SizeType scaled_time_tol = time_reach(widest, time_tol);
    return abs_diff(a.samp_idx, b.samp_idx) <= scaled_time_tol &&
           abs_diff(a.filter_idx, b.filter_idx) <= filter_tol &&
           abs_diff(a.dm_idx, b.dm_idx) <= dm_tol;
}

bool are_coincident(const Giant& a,
                    const Giant& b,
                    const ClusterTolerances& tol) noexcept {
    if (!are_coincident(a, b, tol.time_tol, tol.filter_tol, tol.dm_tol)) {
        return false;
    }
    if (!(tol.dm_smear > 0.0) || !(tol.tsamp > 0.0) ||
        abs_diff(a.dm_idx, b.dm_idx) <= 1) {
        return true;
    }
    const double step =
        subband_dm_step(a.f_low, a.f_high, static_cast<float>(tol.tsamp));
    if (!(step > 0.0)) {
        return true; // no dispersive delay to scale by
    }
    const double widest = static_cast<double>(
        std::max(giant_boxcar_width(a), giant_boxcar_width(b)));
    const double ddm =
        std::abs(static_cast<double>(a.dm) - static_cast<double>(b.dm));
    double growth = 1.0;
    if (tol.snr_floor > 0.0) {
        const double ratio =
            static_cast<double>(std::max(a.snr, b.snr)) / tol.snr_floor;
        growth = std::max(1.0, std::sqrt(std::max(0.0, (ratio * ratio) - 1.0)));
    }
    return ddm <= tol.dm_smear * widest * step * growth;
}

namespace {

SizeType find_root(std::vector<SizeType>& parent, SizeType x) {
    while (parent[x] != x) {
        parent[x] = parent[parent[x]]; // path halving
        x         = parent[x];
    }
    return x;
}

void unite(std::vector<SizeType>& parent, SizeType a, SizeType b) {
    a = find_root(parent, a);
    b = find_root(parent, b);
    if (a == b) {
        return;
    }
    // Keep the smaller index as root: the root is then always the smallest
    // index of its component, so the labels do not depend on the order in
    // which pairs are visited.
    if (a < b) {
        parent[b] = a;
    } else {
        parent[a] = b;
    }
}

// Largest sample distance at which two giants can coincide: the time
// tolerance scales with the wider boxcar of the pair.
SizeType max_time_reach(std::span<const Giant> candidates, SizeType time_tol) {
    SizeType widest = 1;
    for (const auto& g : candidates) {
        widest = std::max(widest, giant_boxcar_width(g));
    }
    return time_reach(widest, time_tol);
}

} // namespace

std::vector<SizeType>
ClusterLabelPlan::execute(std::span<const Giant> candidates,
                          const ClusterTolerances& tol) const {
    const SizeType n = candidates.size();
    std::vector<SizeType> labels(n);
    std::iota(labels.begin(), labels.end(), SizeType{0});
    if (n < 2) {
        return labels;
    }
    const SizeType reach = max_time_reach(candidates, tol.time_tol);

    // Indices by sample, so a giant only meets those inside the window.
    std::vector<SizeType> by_time(n);
    std::iota(by_time.begin(), by_time.end(), SizeType{0});
    std::stable_sort(by_time.begin(), by_time.end(),
                     [&](SizeType a, SizeType b) {
                         return candidates[a].samp_idx < candidates[b].samp_idx;
                     });
    const auto samp = [&](SizeType rank) {
        return candidates[by_time[rank]].samp_idx;
    };

    if (m_mode == ClusterMode::kChain) {
        // Pair (i, j) is tested once, from the wider giant (ties: the
        // lower rank), over that giant's own time window. The window of a
        // narrow giant then never grows to that of the widest boxcar, and
        // the scan streams over compact time-ordered arrays.
        std::vector<SizeType> r_samp(n);
        std::vector<SizeType> r_dm(n);
        std::vector<SizeType> r_filter(n);
        std::vector<SizeType> r_width(n);
        for (SizeType r = 0; r < n; ++r) {
            const Giant& g = candidates[by_time[r]];
            r_samp[r]      = static_cast<SizeType>(g.samp_idx);
            r_dm[r]        = static_cast<SizeType>(g.dm_idx);
            r_filter[r]    = static_cast<SizeType>(g.filter_idx);
            r_width[r]     = giant_boxcar_width(g);
        }
        std::vector<SizeType> parent = labels;
        const auto test              = [&](SizeType r, SizeType q) {
            const auto ddm =
                r_dm[r] > r_dm[q] ? r_dm[r] - r_dm[q] : r_dm[q] - r_dm[r];
            const auto dfl = r_filter[r] > r_filter[q]
                                 ? r_filter[r] - r_filter[q]
                                 : r_filter[q] - r_filter[r];
            if (ddm > tol.dm_tol || dfl > tol.filter_tol) {
                return;
            }
            const SizeType i = by_time[r];
            const SizeType j = by_time[q];
            // Already one cluster: the (costlier) full test cannot change it.
            if (find_root(parent, i) != find_root(parent, j) &&
                are_coincident(candidates[i], candidates[j], tol)) {
                unite(parent, i, j);
            }
        };
        for (SizeType r = 0; r < n; ++r) {
            const SizeType w       = r_width[r];
            const SizeType own     = time_reach(w, tol.time_tol);
            const SizeType reach_r = std::min(own, reach);
            for (SizeType q = r + 1; q < n; ++q) {
                if (r_samp[q] - r_samp[r] > reach_r) {
                    break;
                }
                if (r_width[q] <= w) {
                    test(r, q);
                }
            }
            for (SizeType q = r; q-- > 0;) {
                if (r_samp[r] - r_samp[q] > reach_r) {
                    break;
                }
                if (r_width[q] < w) {
                    test(r, q);
                }
            }
        }
        for (SizeType i = 0; i < n; ++i) {
            labels[i] = find_root(parent, i);
        }
        return labels;
    }

    // Seeded: brightest first, ties by index.
    std::vector<SizeType> by_snr(n);
    std::iota(by_snr.begin(), by_snr.end(), SizeType{0});
    std::stable_sort(by_snr.begin(), by_snr.end(), [&](SizeType a, SizeType b) {
        return candidates[a].snr > candidates[b].snr;
    });
    std::vector<uint8_t> assigned(n, 0);
    // Rank of each giant in `by_time`, to start the window at the seed.
    std::vector<SizeType> rank_of(n);
    for (SizeType r = 0; r < n; ++r) {
        rank_of[by_time[r]] = r;
    }
    for (const SizeType seed : by_snr) {
        if (assigned[seed] != 0) {
            continue;
        }
        assigned[seed]      = 1;
        labels[seed]        = seed;
        const SizeType rank = rank_of[seed];
        const SizeType s0   = candidates[seed].samp_idx;
        const auto claim    = [&](SizeType q) {
            const SizeType j = by_time[q];
            if (assigned[j] == 0 &&
                are_coincident(candidates[seed], candidates[j], tol)) {
                assigned[j] = 1;
                labels[j]   = seed;
            }
        };
        for (SizeType q = rank + 1; q < n && samp(q) - s0 <= reach; ++q) {
            claim(q);
        }
        for (SizeType q = rank; q-- > 0 && s0 - samp(q) <= reach;) {
            claim(q);
        }
    }
    return labels;
}

} // namespace frost::search
