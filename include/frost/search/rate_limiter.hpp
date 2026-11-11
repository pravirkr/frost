#pragma once

#include <frost/common/policies.hpp>

namespace frost::search {

/**
 * Counts giants per minute for a threshold circuit breaker. A non-positive
 * `max_giant_rate` disables it. CandidatePostprocessPlan uses one limiter
 * per plane/call: the row crossing the threshold is kept, and later incoming
 * rows are skipped. Already retained members are not limited again. This
 * bounds subsequent host reduction, not the fused FDMT engine's work.
 */
class RateLimiter {
public:
    explicit RateLimiter(float max_giant_rate);

    /** Records `count` new giants found over `duration_seconds` of data. */
    void record(SizeType count, double duration_seconds);

    /** True once the accumulated rate exceeds `max_giant_rate`. */
    bool exceeded() const;

    void reset();

private:
    float m_max_giant_rate;
    SizeType m_total_giants = 0;
    double m_total_seconds  = 0.0;
};

} // namespace frost::search
