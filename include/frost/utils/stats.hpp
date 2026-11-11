#pragma once

#include <memory>
#include <span>
#include <string_view>

#include <frost/common/policies.hpp>

namespace frost::stats {

// Plan for estimating the scale (variability) or standard deviation of an
// array.
// All three "Plan" classes below use the pimpl idiom, and all three
// declare their destructor/move operations out-of-line (defined in
// stats_cpu.cpp with `= default`): the implicitly-generated versions would
// need Impl's complete type, which isn't visible to callers outside
// stats_cpu.cpp. Copying is disabled since Impl has no clone support.
class EstimateScalePlan {
public:
    /** "mad" estimates zero-centered Gaussian scale as median(abs(x)) /
     * 0.67448975 over every sample (even lengths use the midpoint).
     * "mad_robust" first estimates the location, while "iqr" and "std"
     * use their respective dispersion statistics. Input must be finite and
     * nonempty, and is unchanged. */
    explicit EstimateScalePlan(std::string_view method = "mad");
    EstimateScalePlan(const EstimateScalePlan&)            = delete;
    EstimateScalePlan& operator=(const EstimateScalePlan&) = delete;
    EstimateScalePlan(EstimateScalePlan&&) noexcept;
    EstimateScalePlan& operator=(EstimateScalePlan&&) noexcept;
    ~EstimateScalePlan();
    float execute(const float* __restrict__ data, SizeType count) const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

class MatchedFilterPlan {
public:
    MatchedFilterPlan();
    MatchedFilterPlan(const MatchedFilterPlan&)            = delete;
    MatchedFilterPlan& operator=(const MatchedFilterPlan&) = delete;
    MatchedFilterPlan(MatchedFilterPlan&&) noexcept;
    MatchedFilterPlan& operator=(MatchedFilterPlan&&) noexcept;
    ~MatchedFilterPlan();
    /** Copies a prefix sum for nonempty finite input. Requires
     * 1 <= max_width <= count; invalid geometry throws without changing
     * the preceding prepared state. */
    void prepare(const float* __restrict__ in,
                 SizeType count,
                 SizeType max_width) const;
    void prepare(std::span<const float> in, SizeType max_width) const;
    /** Searches a prepared input with 1 <= filter_width <= max_width and
     * tscrunch > 0. Writes ceil((count-max_width+1)/tscrunch) samples;
     * the pointer overload requires that much output storage. The span
     * overload checks capacity before writing. */
    void execute(float* __restrict__ out,
                 SizeType filter_width,
                 SizeType tscrunch = 1) const;
    void execute(std::span<float> out,
                 SizeType filter_width,
                 SizeType tscrunch = 1) const;

    /**
     * Inclusive prefix of the last `prepare` input. `data[0]` is 0 and
     * `data[i]` is the sum of the first `i` samples. Valid until the next
     * `prepare`.
     */
    struct Prefix {
        const float* data  = nullptr;
        SizeType count     = 0;
        SizeType max_width = 0;
    };
    Prefix prefix() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

// This algorithm works by scrunching the data down to a time resolution
//    representative of the desired smoothing length and then stretching
//    it back out again. The scrunching is done using the median-of-5
//    to ensure robustness against outliers (e.g., strong RFI spikes).
class BaselineFilterPlan {
public:
    BaselineFilterPlan();
    BaselineFilterPlan(const BaselineFilterPlan&)            = delete;
    BaselineFilterPlan& operator=(const BaselineFilterPlan&) = delete;
    BaselineFilterPlan(BaselineFilterPlan&&) noexcept;
    BaselineFilterPlan& operator=(BaselineFilterPlan&&) noexcept;
    ~BaselineFilterPlan();
    void execute(float* __restrict__ data,
                 SizeType count,
                 SizeType smooth_radius) const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

// Smoothing radius in samples for BaselineFilterPlan: baseline_length /
// (2 tsamp scrunch), computed in double (a float tsamp of 1 ms turns a 2 s
// window into 999 samples) and clamped to [1, count / 2] so that a series of
// at least 4 samples always gets at least 2 baseline knots. With one knot
// BaselineFilterPlan does nothing and the FDMT DC level stays in the row.
[[nodiscard]] SizeType baseline_smooth_radius(float baseline_length,
                                              double tsamp,
                                              SizeType scrunch,
                                              SizeType count);

// Helper functions
// Estimate the scale (variability) or standard deviation of an array.
float estimate_scale(const float* __restrict__ data,
                     SizeType count,
                     std::string_view method);

/** Clips each centered window to the input and writes its first out_size
 * positions (out_size <= in_size). Overlap is supported. Mean windows use
 * floor(width/2) samples behind and ceil(width/2) including the current
 * sample ahead. Median widths must be positive/odd; widths 3/5 average
 * the central pair at clipped even edges, other widths use the upper median.
 * Empty output is a no-op after validating geometry and method. */
void running_filter(const float* in,
                    SizeType in_size,
                    float* out,
                    SizeType out_size,
                    SizeType window_size,
                    std::string_view method = "median");

} // namespace frost::stats
