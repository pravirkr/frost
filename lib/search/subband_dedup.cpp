#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include <dmt/dmt.hpp>

#include <frost/common/numeric.hpp>
#include <frost/search/subband_dedup.hpp>

namespace frost::search {

namespace {

SizeType find_root(std::vector<SizeType>& parent, SizeType x) {
    while (parent[x] != x) {
        parent[x] = parent[parent[x]];
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
    if (a < b) {
        parent[b] = a;
    } else {
        parent[a] = b;
    }
}

SizeType abs_diff(SizeType a, SizeType b) noexcept {
    return (a > b) ? (a - b) : (b - a);
}

SizeType channel_span(const Giant& g) noexcept {
    if (g.chan_end < g.chan_begin) {
        return 0;
    }
    return (g.chan_end - g.chan_begin) + 1;
}

SizeType boxcar_width(const Giant& g) noexcept { return giant_boxcar_width(g); }

bool contains_channels(const Giant& outer, const Giant& inner) noexcept {
    return outer.chan_begin <= inner.chan_begin &&
           outer.chan_end >= inner.chan_end;
}

// A narrow band whose DM step is this many times the wide band's cannot
// confirm the DM, so a match also needs a consistent S/N.
constexpr double kPoorDmStepRatio = 4.0;
// A flat-spectrum event seen at S/N `s` in the wide band reaches about
// `s * sqrt(span_narrow / span_wide)` in the narrow one. Allow this
// factor plus a constant for spectral structure and noise.
constexpr double kSnrConsistencyFactor = 2.0;
constexpr double kSnrConsistencySlack  = 3.0;

// One step is one sample of delay across the band it was computed on.
// Pass 1 keeps the max-S/N trial, which can sit a few boxcars of that
// delay off the other row's DM (`dm_smear` of them; one when it is off,
// and never less than two samples). A NaN DM fails the gate.
bool dm_within_smear(const Giant& a,
                     const Giant& b,
                     const ClusterTolerances& tol,
                     double widest,
                     double step) noexcept {
    const double smear         = tol.dm_smear > 0.0 ? tol.dm_smear : 1.0;
    const double delay_samples = std::max(2.0, widest);
    const double dm_delta =
        std::abs(static_cast<double>(a.dm) - static_cast<double>(b.dm));
    return dm_delta <= (smear * delay_samples * step);
}

// Wider DM, narrower band's frequency span. Times are compared in
// double after the analytic shift so the residual is not an integer
// truncation.
bool on_same_curve(const Giant& a,
                   const Giant& b,
                   const ClusterTolerances& tol,
                   float tsamp) noexcept {
    const SizeType span_a = channel_span(a);
    const SizeType span_b = channel_span(b);
    if (span_a == 0 || span_b == 0) {
        return false;
    }
    if (span_a == span_b) {
        // The same extent from two planes is one FDMT node searched
        // twice (a copied leftover child). Within one plane, pass 1
        // already decided these rows are distinct events. `dm_idx` is a
        // trial on one plane's grid, so the DMs are compared physically.
        if (a.plane == b.plane || a.chan_begin != b.chan_begin ||
            a.chan_end != b.chan_end ||
            abs_diff(a.filter_idx, b.filter_idx) > tol.filter_tol) {
            return false;
        }
        const double step = subband_dm_step(a.f_low, a.f_high, tsamp);
        const double widest =
            static_cast<double>(std::max(boxcar_width(a), boxcar_width(b)));
        return is_positive_finite(step) &&
               dm_within_smear(a, b, tol, widest, step) &&
               static_cast<double>(abs_diff(a.samp_idx, b.samp_idx)) <=
                   static_cast<double>(tol.time_tol) * widest;
    }
    const bool nested = contains_channels(a, b) || contains_channels(b, a);
    if (!nested) {
        return false;
    }
    const Giant& wide   = span_a > span_b ? a : b;
    const Giant& narrow = span_a > span_b ? b : a;
    if (abs_diff(wide.filter_idx, narrow.filter_idx) > tol.filter_tol) {
        return false;
    }
    const double step = subband_dm_step(narrow.f_low, narrow.f_high, tsamp);
    if (!is_positive_finite(step)) {
        return false;
    }
    if (!dm_within_smear(wide, narrow, tol,
                         static_cast<double>(std::max(boxcar_width(wide),
                                                      boxcar_width(narrow))),
                         step)) {
        return false;
    }
    const double wide_step = subband_dm_step(wide.f_low, wide.f_high, tsamp);
    if (is_positive_finite(wide_step) && step > kPoorDmStepRatio * wide_step) {
        const double expected =
            static_cast<double>(wide.snr) *
            std::sqrt(static_cast<double>(span_a < span_b ? span_a : span_b) /
                      static_cast<double>(std::max(span_a, span_b)));
        if (static_cast<double>(narrow.snr) >
            (kSnrConsistencyFactor * expected) + kSnrConsistencySlack) {
            return false;
        }
    }
    const double shift =
        subband_time_shift_samples(wide.dm, wide.f_low, narrow.f_low, tsamp);
    const double aligned = static_cast<double>(narrow.samp_idx) + shift;
    const double delta = std::abs(aligned - static_cast<double>(wide.samp_idx));
    const double widest =
        static_cast<double>(std::max(boxcar_width(wide), boxcar_width(narrow)));
    const double time_window = static_cast<double>(tol.time_tol) * widest;
    return delta <= time_window;
}

bool wider_extent(const Giant& cand, const Giant& best) noexcept {
    const SizeType span_c = channel_span(cand);
    const SizeType span_b = channel_span(best);
    if (span_c != span_b) {
        return span_c > span_b;
    }
    return cand.snr > best.snr;
}

} // namespace

double subband_time_shift_samples(float dm,
                                  float f_wide,
                                  float f_narrow,
                                  float tsamp) noexcept {
    if (!is_positive_finite(tsamp) || !is_positive_finite(f_wide) ||
        !is_positive_finite(f_narrow)) {
        return 0.0;
    }
    const double wide   = static_cast<double>(f_wide);
    const double narrow = static_cast<double>(f_narrow);
    const double inv_f2 = (1.0 / (wide * wide)) - (1.0 / (narrow * narrow));
    return (static_cast<double>(dm) * static_cast<double>(dmt::kDispConst) *
            inv_f2) /
           static_cast<double>(tsamp);
}

double subband_dm_step(float f_low, float f_high, float tsamp) noexcept {
    if (!is_positive_finite(tsamp) || !is_positive_finite(f_low) ||
        !is_finite(f_high) || !(f_high > f_low)) {
        return 0.0;
    }
    const double lo    = static_cast<double>(f_low);
    const double hi    = static_cast<double>(f_high);
    const double denom = (1.0 / (lo * lo)) - (1.0 / (hi * hi));
    if (!is_positive_finite(denom)) {
        return 0.0;
    }
    return static_cast<double>(tsamp) /
           (static_cast<double>(dmt::kDispConst) * denom);
}

std::vector<Giant> SubbandDedupPlan::execute(std::span<const Giant> candidates,
                                             const ClusterTolerances& tol,
                                             float tsamp) const {
    const SizeType n = candidates.size();
    if (n == 0) {
        return {};
    }
    if (!is_positive_finite(tsamp)) {
        throw std::invalid_argument("sub-band dedup requires tsamp > 0");
    }

    std::vector<SizeType> group_of(n);
    if (m_mode == SubbandDedupMode::kTransitive) {
        // Union-find over every matching pair: a quarter reaches the full
        // band through the half it nests in, and a nested row matching two
        // wide rows joins them (on real data mostly DM wings of one pulse).
        std::vector<SizeType> parent(n);
        for (SizeType i = 0; i < n; ++i) {
            parent[i] = i;
        }
        for (SizeType i = 0; i < n; ++i) {
            for (SizeType j = i + 1; j < n; ++j) {
                if (on_same_curve(candidates[i], candidates[j], tol, tsamp)) {
                    unite(parent, i, j);
                }
            }
        }
        for (SizeType i = 0; i < n; ++i) {
            group_of[i] = find_root(parent, i);
        }
    } else {
        // Rows are placed widest first (then brightest, then input order).
        // A row joins the group of the first placed row it matches, or
        // starts a group. Groups never merge, so a nested row cannot
        // bridge two wide rows that pass 1 kept apart.
        std::vector<SizeType> order(n);
        for (SizeType i = 0; i < n; ++i) {
            order[i] = i;
        }
        std::ranges::stable_sort(order, [&](SizeType x, SizeType y) {
            const SizeType sx = channel_span(candidates[x]);
            const SizeType sy = channel_span(candidates[y]);
            if (sx != sy) {
                return sx > sy;
            }
            return candidates[x].snr > candidates[y].snr;
        });
        for (SizeType k = 0; k < n; ++k) {
            const SizeType i = order[k];
            group_of[i]      = i;
            for (SizeType m = 0; m < k; ++m) {
                const SizeType j = order[m];
                if (on_same_curve(candidates[j], candidates[i], tol, tsamp)) {
                    group_of[i] = group_of[j];
                    break;
                }
            }
        }
    }

    // Label each group by its lowest input index, so rows come out in the
    // order of their earliest member.
    std::vector<SizeType> first_member(n, n);
    for (SizeType i = 0; i < n; ++i) {
        SizeType& first = first_member[group_of[i]];
        first           = std::min(first, i);
    }

    struct Group {
        Giant winner;
        float best_snr      = 0.0F;
        SizeType best_begin = 0;
        SizeType best_end   = 0;
        CandidateLocation best_location;
        std::set<std::pair<SizeType, SizeType>> extents;
    };
    std::map<SizeType, Group> best;
    for (SizeType i = 0; i < n; ++i) {
        const auto& cand      = candidates[i];
        const SizeType label  = first_member[group_of[i]];
        auto [it, inserted]   = best.try_emplace(label);
        Group& group          = it->second;
        const float cand_best = cand.best_snr();
        const SizeType cand_b =
            cand.has_best_extent() ? cand.best_chan_begin : cand.chan_begin;
        const SizeType cand_e =
            cand.has_best_extent() ? cand.best_chan_end : cand.chan_end;
        group.extents.emplace(cand.chan_begin, cand.chan_end);
        if (inserted) {
            group.winner        = cand;
            group.best_snr      = cand_best;
            group.best_begin    = cand_b;
            group.best_end      = cand_e;
            group.best_location = cand.strongest_location();
            continue;
        }
        const auto members_sum = group.winner.members + cand.members;
        if (wider_extent(cand, group.winner)) {
            group.winner = cand;
        }
        group.winner.members = members_sum;
        if (cand_best > group.best_snr) {
            group.best_snr      = cand_best;
            group.best_begin    = cand_b;
            group.best_end      = cand_e;
            group.best_location = cand.strongest_location();
        }
    }

    std::vector<Giant> merged;
    merged.reserve(best.size());
    for (auto& [label, group] : best) {
        Giant out           = group.winner;
        out.n_extents       = group.extents.size();
        out.snr_best        = group.best_snr;
        out.best_chan_begin = group.best_begin;
        out.best_chan_end   = group.best_end;
        out.location_best   = group.best_location;
        merged.push_back(out);
    }
    return merged;
}

} // namespace frost::search
