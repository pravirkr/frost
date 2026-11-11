// Manual channel-zap and IQRM channel masking (docs/rfi.md): standalone
// static/dynamic masking strategies, with no dependency on RFICleaner::Impl
// state.

#include <algorithm>
#include <stdexcept>
#include <vector>

#include <frost/common/numeric.hpp>
#include <frost/prepare/rfi.hpp>

#include "lib/cpu/rfi_kernels.hpp"

namespace frost::prepare::rfi {

void apply_manual_killmask(std::span<uint8_t> killmask,
                           std::span<const ChannelRange> channel_zaps) {
    if (killmask.empty()) {
        return;
    }
    for (const auto& range : channel_zaps) {
        const auto end = std::min(range.end, killmask.size() - 1);
        for (SizeType c = range.start; c <= end; ++c) {
            killmask[c] = 0;
        }
    }
}

std::vector<uint8_t> iqrm_mask(std::span<const float> x,
                               SizeType radius,
                               float threshold,
                               std::span<const uint8_t> ignore) {
    const SizeType n = x.size();
    if (radius == 0) {
        throw std::invalid_argument("iqrm_mask: radius must be > 0");
    }
    if (!is_positive_finite(threshold)) {
        throw std::invalid_argument("iqrm_mask: threshold must be > 0");
    }
    if (!ignore.empty() && ignore.size() != n) {
        throw std::invalid_argument("iqrm_mask: ignore size must match x");
    }
    std::vector<uint8_t> mask(n, 0);
    if (n == 0) {
        return mask;
    }
    const auto ignored = [&](SizeType i) {
        return !ignore.empty() && ignore[i] != 0;
    };

    // Ignored channels behave as the reference's NaNs: their lagged
    // differences are excluded from the quantiles and cannot cast votes.
    std::vector<std::vector<SizeType>> cast(n);
    std::vector<std::vector<SizeType>> received(n);
    std::vector<double> diff(n);
    std::vector<uint8_t> valid(n);
    std::vector<double> sorted;
    const auto ni          = static_cast<IndexType>(n);
    const auto clamp_index = [&](IndexType i) {
        return static_cast<SizeType>(std::clamp<IndexType>(i, 0, ni - 1));
    };
    for (SizeType lag = 1; lag <= radius;
         lag          = std::max(lag + lag / 2, lag + 1)) {
        for (const IndexType k :
             {static_cast<IndexType>(lag), -static_cast<IndexType>(lag)}) {
            sorted.clear();
            for (IndexType i = 0; i < ni; ++i) {
                const SizeType j = clamp_index(i - k);
                const auto ui    = static_cast<SizeType>(i);
                valid[ui] = static_cast<uint8_t>(!ignored(ui) && !ignored(j));
                diff[ui] =
                    static_cast<double>(x[ui]) - static_cast<double>(x[j]);
                if (valid[ui] != 0) {
                    sorted.push_back(diff[ui]);
                }
            }
            if (sorted.empty()) {
                continue;
            }
            std::sort(sorted.begin(), sorted.end());
            const double q1  = kernels::quantile_sorted(sorted, 0.25);
            const double med = kernels::quantile_sorted(sorted, 0.5);
            const double q3  = kernels::quantile_sorted(sorted, 0.75);
            const double cut =
                static_cast<double>(threshold) * (q3 - q1) / 1.349;
            for (IndexType i = 0; i < ni; ++i) {
                const auto ui = static_cast<SizeType>(i);
                if (valid[ui] != 0 && diff[ui] - med > cut) {
                    const SizeType j = clamp_index(i - k);
                    cast[j].push_back(ui);
                    received[ui].push_back(j);
                }
            }
        }
        // With edge clamping all larger lags repeat these comparisons.
        // Votes are sets, so repetitions cannot change the final mask.
        // Stopping here also keeps an arbitrary requested radius from
        // overflowing the lag-growth arithmetic or signed index casts.
        if (lag >= n - 1)
            break;
    }
    for (auto& v : cast) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }
    for (SizeType i = 0; i < n; ++i) {
        auto& casters = received[i];
        std::sort(casters.begin(), casters.end());
        casters.erase(std::unique(casters.begin(), casters.end()),
                      casters.end());
        for (const SizeType j : casters) {
            if (cast[j].size() < casters.size()) {
                mask[i] = 1;
                break;
            }
        }
        if (ignored(i)) {
            mask[i] = 1;
        }
    }
    return mask;
}

} // namespace frost::prepare::rfi
