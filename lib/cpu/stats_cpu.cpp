#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <frost/common/numeric.hpp>
#include <frost/common/policies.hpp>
#include <frost/utils/stats.hpp>

#include "lib/cpu/stats_kernels.hpp"

namespace frost::stats {

class EstimateScalePlan::Impl {
public:
    explicit Impl(std::string_view method = "mad")
        : m_method(string_to_method(method)) {}

    float execute(const float* __restrict__ data, SizeType count) {
        if (count == 0 || data == nullptr) {
            throw std::invalid_argument(
                "Data array must have at least one element");
        }
        switch (m_method) {
        case Method::kMad:
            return compute_mad(data, count);
        case Method::kMadRobust:
            return compute_mad_robust(data, count);
        case Method::kIqr:
            return compute_iqr(data, count);
        case Method::kStd:
            return compute_std(data, count);
        }
        return 0.0F;
    }

private:
    static constexpr float kIqrNorm = 1.3489795003921634F;
    static constexpr float kMadNorm = 0.6744897501960817F;
    static constexpr float kAadNorm = 0.7978845608028654F; // sqrt(2 / pi)

    enum class Method : std::uint8_t {
        kMad,
        kMadRobust,
        kIqr,
        kStd,
    };
    Method m_method;
    std::vector<float> m_buf1;
    std::vector<float> m_buf2;

    static Method string_to_method(std::string_view method) {
        if (method == "mad") {
            return Method::kMad;
        }
        if (method == "mad_robust") {
            return Method::kMadRobust;
        }
        if (method == "iqr") {
            return Method::kIqr;
        }
        if (method == "std") {
            return Method::kStd;
        }
        throw std::invalid_argument(
            fmt::format("Unknown scale method: {}. Valid options are: mad, "
                        "mad_robust, iqr, std",
                        method));
    }
    float compute_mad(const float* __restrict__ data, SizeType count) {
        auto abs_view =
            std::views::transform(std::ranges::subrange(data, data + count),
                                  [](float val) { return std::abs(val); });
        m_buf1.assign(abs_view.begin(), abs_view.end());
        // A median-of-five reduction both approximates the median and drops
        // remainders at each level (8192 used only its first 3125 samples).
        // Select over the entire series so scale does not depend on ordering.
        auto middle = m_buf1.begin() + static_cast<IndexType>(count / 2);
        std::nth_element(m_buf1.begin(), middle, m_buf1.end());
        float med_abs_dev = *middle;
        if (count % 2 == 0) {
            const float lower = *std::max_element(m_buf1.begin(), middle);
            med_abs_dev       = 0.5F * lower + 0.5F * med_abs_dev;
        }
        return med_abs_dev / kMadNorm;
    }

    float compute_mad_robust(const float* __restrict__ data, SizeType count) {
        m_buf1.assign(data, data + count);
        std::nth_element(m_buf1.begin(),
                         m_buf1.begin() + static_cast<IndexType>(count / 2),
                         m_buf1.end());
        const auto median = m_buf1[count / 2];
        m_buf2.resize(count);
        for (size_t i = 0; i < count; ++i) {
            m_buf2[i] = std::abs(data[i] - median);
        }
        std::nth_element(m_buf2.begin(),
                         m_buf2.begin() + static_cast<IndexType>(count / 2),
                         m_buf2.end());
        const auto mad = m_buf2[count / 2] / kMadNorm;
        if (mad == 0.0F) {
            // Calculate the mean absolute deviation
            const auto sum =
                std::accumulate(m_buf2.begin(), m_buf2.end(), 0.0F);
            return sum / static_cast<float>(count) / kAadNorm;
        }
        return mad;
    }

    float compute_iqr(const float* __restrict__ data, SizeType count) {
        const auto q1_index = count / 4;
        const auto q3_index = count * 3 / 4;

        m_buf1.assign(data, data + count);
        std::nth_element(m_buf1.begin(),
                         m_buf1.begin() + static_cast<IndexType>(q1_index),
                         m_buf1.end());
        const auto q1 = m_buf1[q1_index];

        std::nth_element(m_buf1.begin(),
                         m_buf1.begin() + static_cast<IndexType>(q3_index),
                         m_buf1.end());
        const auto q3  = m_buf1[q3_index];
        const auto iqr = q3 - q1;
        return iqr / kIqrNorm;
    }

    static float compute_std(const float* __restrict__ data, SizeType count) {
        const double origin = data[0];
        double sum_delta    = 0;
        for (SizeType i = 0; i < count; ++i)
            sum_delta += static_cast<double>(data[i]) - origin;
        const double mean_delta  = sum_delta / static_cast<double>(count);
        double squared_deviation = 0;
        for (SizeType i = 0; i < count; ++i) {
            const double delta =
                (static_cast<double>(data[i]) - origin) - mean_delta;
            squared_deviation += delta * delta;
        }
        return static_cast<float>(
            std::sqrt(squared_deviation / static_cast<double>(count)));
    }
};

class MatchedFilterPlan::Impl {
    std::vector<float> m_scanned;
    SizeType m_max_width = 0;

public:
    void
    prepare(const float* __restrict__ in, SizeType count, SizeType max_width) {
        if (in == nullptr || count == 0 || max_width == 0 ||
            max_width > count ||
            count == std::numeric_limits<SizeType>::max()) {
            throw std::invalid_argument("MatchedFilterPlan: require nonempty "
                                        "input and 1 <= max_width <= count");
        }
        m_scanned.resize(count + 1);
        m_scanned[0] = 0.0F;
        std::inclusive_scan(in, in + count, m_scanned.begin() + 1);
        m_max_width = max_width;
    }

    void
    execute(float* __restrict__ out,
            SizeType filter_width,
            SizeType tscrunch,
            SizeType capacity = std::numeric_limits<SizeType>::max()) const {
        if (m_scanned.empty())
            throw std::logic_error(
                "MatchedFilterPlan: prepare must precede execute");
        if (out == nullptr || filter_width == 0 || filter_width > m_max_width ||
            tscrunch == 0) {
            throw std::invalid_argument(
                "MatchedFilterPlan: invalid output, filter width or tscrunch");
        }
        const SizeType out_count = m_scanned.size() - m_max_width;
        const SizeType required  = 1 + (out_count - 1) / tscrunch;
        if (capacity < required)
            throw std::invalid_argument(
                "MatchedFilterPlan: output span is too small");
        const SizeType offset = m_max_width / 2;
        const SizeType ahead  = ((filter_width - 1) / 2) + 1;
        const SizeType behind = filter_width / 2;

        for (SizeType i = 0; i < out_count; i += tscrunch) {
            const SizeType idx = offset + i;
            out[i / tscrunch] =
                m_scanned[idx + ahead] - m_scanned[idx - behind];
        }
    }

    MatchedFilterPlan::Prefix prefix() const {
        MatchedFilterPlan::Prefix out;
        out.data      = m_scanned.data();
        out.count     = m_scanned.empty() ? 0 : m_scanned.size() - 1;
        out.max_width = m_max_width;
        return out;
    }
};

class BaselineFilterPlan::Impl {
    std::vector<float> m_buf1;
    std::vector<float> m_buf2;
    std::vector<float> m_baseline;

public:
    void
    execute(float* __restrict__ data, SizeType count, SizeType smooth_radius) {
        if (smooth_radius == 0) {
            throw std::invalid_argument(
                "BaselineFilterPlan: smooth_radius must be > 0");
        }
        if (count == 0)
            return;
        if (data == nullptr) {
            throw std::invalid_argument("BaselineFilterPlan: null input");
        }
        // Knot count: count / smooth_radius rounded to nearest, in integers.
        // A float lround could differ by one for large counts and is not
        // exact under -ffast-math.
        const auto sample_count =
            count / smooth_radius +
            (count % smooth_radius >= smooth_radius / 2 + smooth_radius % 2);

        if (sample_count <= 1) {
            return; // Nothing to do
        }
        if (sample_count > (std::numeric_limits<SizeType>::max() - 2) / 2) {
            throw std::invalid_argument(
                "BaselineFilterPlan: knot count overflows");
        }
        // As we will use median-of-5, round down to sample_count times a
        // power of five. Integer arithmetic: under -ffast-math pow(5, 4)
        // can truncate to 624, leaving the last scrunch one output short
        // and the final knot reading stale buffer contents.
        SizeType count_round = sample_count;
        while (count_round <= count / 5) {
            count_round *= 5;
        }

        m_buf1.resize(count_round);
        m_buf2.resize(count_round / 5);

        // First we re-sample to the rounded size
        kernels::linear_stretch(data, count, m_buf1.data(), count_round);

        // Then we median-of-5 down to the desired size
        for (size_t size = count_round; size > sample_count; size /= 5) {
            kernels::median_scrunch5(m_buf1.data(), size, m_buf2.data());
            std::swap(m_buf1, m_buf2);
        }

        m_buf2.resize((sample_count * 2) + 2);
        kernels::linear_stretch(m_buf1.data(), sample_count, m_buf2.data() + 1,
                                sample_count * 2);
        m_buf2[0] = 2 * m_buf2[1] - m_buf2[2];
        m_buf2[(sample_count * 2) + 1] =
            2 * m_buf2[sample_count * 2] - m_buf2[(sample_count * 2) - 1];

        m_baseline.resize(count);
        // And finally we stretch back to the original length
        kernels::linear_stretch(m_buf2.data(), (sample_count * 2) + 2,
                                m_baseline.data(), count);
#pragma omp parallel for
        for (size_t i = 0; i < count; ++i) {
            data[i] -= m_baseline[i];
        }
    }
};

SizeType baseline_smooth_radius(float baseline_length,
                                double tsamp,
                                SizeType scrunch,
                                SizeType count) {
    if (!is_positive_finite(baseline_length) || !is_positive_finite(tsamp) ||
        scrunch == 0) {
        throw std::invalid_argument(
            "baseline_smooth_radius: baseline_length, tsamp and scrunch must "
            "be positive");
    }
    const double r = static_cast<double>(baseline_length) /
                     (2.0 * tsamp * static_cast<double>(scrunch));
    // The relative nudge keeps exact quotients (e.g. 2 s / 2 ms = 1000)
    // from landing at 999.99999 after the float to double conversion.
    auto radius =
        static_cast<SizeType>(std::min(std::floor(r * (1.0 + 1.0e-6)), 4.0e18));
    radius = std::max<SizeType>(radius, 1);
    if (count >= 4) {
        radius = std::min<SizeType>(radius, count / 2);
    }
    return radius;
}

EstimateScalePlan::EstimateScalePlan(std::string_view method)
    : m_impl(std::make_unique<Impl>(method)) {}

EstimateScalePlan::~EstimateScalePlan()                            = default;
EstimateScalePlan::EstimateScalePlan(EstimateScalePlan&&) noexcept = default;
EstimateScalePlan&
EstimateScalePlan::operator=(EstimateScalePlan&&) noexcept = default;

float EstimateScalePlan::execute(const float* __restrict__ data,
                                 SizeType count) const {
    return m_impl->execute(data, count);
}

MatchedFilterPlan::MatchedFilterPlan() : m_impl(std::make_unique<Impl>()) {}
MatchedFilterPlan::~MatchedFilterPlan()                            = default;
MatchedFilterPlan::MatchedFilterPlan(MatchedFilterPlan&&) noexcept = default;
MatchedFilterPlan&
MatchedFilterPlan::operator=(MatchedFilterPlan&&) noexcept = default;

void MatchedFilterPlan::prepare(const float* __restrict__ in,
                                SizeType count,
                                SizeType max_width) const {
    m_impl->prepare(in, count, max_width);
}
void MatchedFilterPlan::prepare(std::span<const float> in,
                                SizeType max_width) const {
    m_impl->prepare(in.data(), in.size(), max_width);
}

void MatchedFilterPlan::execute(float* __restrict__ out,
                                SizeType filter_width,
                                SizeType tscrunch) const {
    m_impl->execute(out, filter_width, tscrunch);
}
void MatchedFilterPlan::execute(std::span<float> out,
                                SizeType filter_width,
                                SizeType tscrunch) const {
    m_impl->execute(out.data(), filter_width, tscrunch, out.size());
}

MatchedFilterPlan::Prefix MatchedFilterPlan::prefix() const {
    return m_impl->prefix();
}

BaselineFilterPlan::BaselineFilterPlan() : m_impl(std::make_unique<Impl>()) {}
BaselineFilterPlan::~BaselineFilterPlan()                             = default;
BaselineFilterPlan::BaselineFilterPlan(BaselineFilterPlan&&) noexcept = default;
BaselineFilterPlan&
BaselineFilterPlan::operator=(BaselineFilterPlan&&) noexcept = default;

void BaselineFilterPlan::execute(float* __restrict__ data,
                                 SizeType count,
                                 SizeType smooth_radius) const {
    m_impl->execute(data, count, smooth_radius);
}

float estimate_scale(const float* __restrict__ data,
                     SizeType count,
                     std::string_view method) {
    EstimateScalePlan plan(method);
    return plan.execute(data, count);
}

void running_filter(const float* in,
                    SizeType in_size,
                    float* out,
                    SizeType out_size,
                    SizeType window_size,
                    std::string_view method) {
    if (out_size > in_size || window_size == 0 ||
        (method == "median" && window_size % 2 == 0) ||
        (method != "median" && method != "mean")) {
        throw std::invalid_argument(
            "running_filter: invalid sizes, window or method");
    }
    if (out_size == 0)
        return;
    if (in == nullptr || out == nullptr)
        throw std::invalid_argument("running_filter: null storage");
    std::vector<float> snapshot;
    const std::less<const float*> less;
    if (less(in, out + out_size) && less(out, in + in_size)) {
        snapshot.assign(in, in + in_size);
        in = snapshot.data();
    }
    if (method == "mean") {
        kernels::running_mean(in, in_size, out, out_size, window_size);
    } else if (method == "median") {
        // For median, if window_size matches one of our optimized kernels,
        // dispatch to the compile-time version; otherwise use generic.
        switch (window_size) {
        case 3:
            kernels::running_median_impl<3>(in, in_size, out, out_size);
            break;
        case 5:
            kernels::running_median_impl<5>(in, in_size, out, out_size);
            break;
        case 7:
            kernels::running_median_impl<7>(in, in_size, out, out_size);
            break;
        default:
            // Optionally, check that window_size is odd.
            if (window_size % 2 == 0) {
                throw std::invalid_argument(
                    "Window size must be odd for median filtering");
            }
            kernels::running_median_generic(in, in_size, out, out_size,
                                            window_size);
            break;
        }
    } else {
        throw std::invalid_argument(
            "Unsupported method. Choose 'median' or 'mean'.");
    }
}

} // namespace frost::stats
