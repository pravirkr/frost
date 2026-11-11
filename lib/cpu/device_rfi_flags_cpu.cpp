#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include <frost/utils/stats.hpp>

#include "lib/prepare/device_rfi.hpp"

namespace frost::pipeline::device {

// Shared zero-DM broadband detector used by RFICleaner (lib/cpu/rfi_cpu.cpp)
// and DeviceRfi (lib/cuda/device_rfi_cuda.cu): baseline removal, MAD
// normalisation, then |x| > threshold at boxcar widths 1, 2, 4, ...,
// boxcar_max, with each detection widened by its boxcar. Kept in a host TU so
// both paths share the exact host -ffast-math numerics.
SizeType broadband_time_flags(std::vector<float> series,
                              SizeType smooth_radius,
                              float threshold,
                              SizeType boxcar_max,
                              std::span<uint8_t> flags) {
    const SizeType n = series.size();
    if (flags.size() < n) {
        throw std::invalid_argument(
            "broadband flags are shorter than the series");
    }
    stats::BaselineFilterPlan baseline;
    baseline.execute(series.data(), n, smooth_radius);
    const float rms = stats::estimate_scale(series.data(), n, "mad");
    if (!(rms > 0.0F)) {
        return 0;
    }
    for (auto& v : series) {
        v /= rms;
    }

    std::vector<uint8_t> mask(n, 0);
    for (SizeType t = 0; t < n; ++t) {
        mask[t] = static_cast<uint8_t>(std::abs(series[t]) > threshold);
    }
    if (n > boxcar_max && boxcar_max > 1) {
        stats::MatchedFilterPlan series_filter;
        series_filter.prepare(series.data(), n, boxcar_max);
        const SizeType filtered_len  = n + 1 - boxcar_max;
        const SizeType filter_offset = boxcar_max / 2;
        std::vector<float> filtered(filtered_len);
        std::vector<float> filtered_mask(n, 0.0F);
        std::vector<float> widened(filtered_len);
        for (SizeType width = 1; width <= boxcar_max; width *= 2) {
            series_filter.execute(filtered.data(), width);
            const float norm = 1.0F / std::sqrt(static_cast<float>(width));
            std::fill(filtered_mask.begin(), filtered_mask.end(), 0.0F);
            for (SizeType i = 0; i < filtered_len; ++i) {
                filtered_mask[filter_offset + i] = static_cast<float>(
                    std::abs(filtered[i] * norm) > threshold);
            }
            stats::MatchedFilterPlan mask_filter;
            mask_filter.prepare(filtered_mask.data(), n, boxcar_max);
            mask_filter.execute(widened.data(), width);
            for (SizeType i = 0; i < filtered_len; ++i) {
                if (widened[i] > 0.5F) {
                    mask[filter_offset + i] = 1;
                }
            }
        }
    }
    SizeType count = 0;
    for (SizeType t = 0; t < n; ++t) {
        if (mask[t] != 0 && flags[t] == 0) {
            flags[t] = 1;
            ++count;
        }
    }
    return count;
}

} // namespace frost::pipeline::device
