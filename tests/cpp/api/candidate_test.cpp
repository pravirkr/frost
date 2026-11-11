#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <frost/common/policies.hpp>
#include <frost/search/candidate.hpp>
#include <frost/search/rate_limiter.hpp>

using frost::SizeType;
using frost::search::are_coincident;
using frost::search::Giant;
using frost::search::RateLimiter;

TEST_CASE("are_coincident scales time tolerance by the wider filter",
          "[giant_finder]") {
    Giant a;
    a.samp_idx   = 1000;
    a.filter_idx = 3; // width 8
    a.dm_idx     = 10;

    Giant b      = a;
    b.samp_idx   = 1000 + (5 * 8); // 40 samples away, within tol*width=5*8
    b.filter_idx = 3;

    CHECK(are_coincident(a, b, /*time_tol=*/5, /*filter_tol=*/1, /*dm_tol=*/2));

    Giant c    = a;
    c.samp_idx = 1000 + (6 * 8); // just outside 5*8
    CHECK_FALSE(
        are_coincident(a, c, /*time_tol=*/5, /*filter_tol=*/1, /*dm_tol=*/2));
}

TEST_CASE("are_coincident rejects candidates too far apart in DM or "
          "filter index",
          "[giant_finder]") {
    Giant a;
    a.samp_idx   = 500;
    a.filter_idx = 0;
    a.dm_idx     = 10;

    Giant far_dm  = a;
    far_dm.dm_idx = 20;
    CHECK_FALSE(are_coincident(a, far_dm, 5, 1, 2));

    Giant far_filter      = a;
    far_filter.filter_idx = 5;
    CHECK_FALSE(are_coincident(a, far_filter, 5, 1, 2));
}

TEST_CASE("RateLimiter trips once the giant rate exceeds the threshold",
          "[rate_limiter]") {
    RateLimiter limiter(/*max_giant_rate=*/60.0F); // 60 giants/minute
    limiter.record(/*count=*/5, /*duration_seconds=*/60.0);
    CHECK_FALSE(limiter.exceeded()); // exactly 5/min, not over

    limiter.record(/*count=*/200, /*duration_seconds=*/60.0);
    CHECK(limiter.exceeded()); // (5+200)/2min = 102.5/min > 60

    limiter.reset();
    CHECK_FALSE(limiter.exceeded());
}

TEST_CASE("RateLimiter is disabled for a non-positive max_giant_rate",
          "[rate_limiter]") {
    RateLimiter limiter(0.0F);
    limiter.record(1000000, 1.0);
    CHECK_FALSE(limiter.exceeded());
}

TEST_CASE("coincidence keeps copied extents in different planes apart",
          "[clustering]") {
    Giant a, b;
    a.snr = b.snr = 10.0F;
    a.chan_end = b.chan_end = 7;
    b.plane                 = 1;
    CHECK_FALSE(are_coincident(a, b, 3, 3, 200));
    b.plane = a.plane;
    CHECK(are_coincident(a, b, 3, 3, 200));
}
