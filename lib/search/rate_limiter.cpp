#include <frost/search/rate_limiter.hpp>

namespace frost::search {

RateLimiter::RateLimiter(float max_giant_rate)
    : m_max_giant_rate(max_giant_rate) {}

void RateLimiter::record(SizeType count, double duration_seconds) {
    m_total_giants += count;
    m_total_seconds += duration_seconds;
}

bool RateLimiter::exceeded() const {
    if (m_max_giant_rate <= 0.0F || m_total_seconds <= 0.0) {
        return false;
    }
    const double minutes = m_total_seconds / 60.0;
    const double rate    = static_cast<double>(m_total_giants) / minutes;
    return rate > static_cast<double>(m_max_giant_rate);
}

void RateLimiter::reset() {
    m_total_giants  = 0;
    m_total_seconds = 0.0;
}

} // namespace frost::search
