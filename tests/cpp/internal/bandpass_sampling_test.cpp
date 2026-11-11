#include "lib/prepare/bandpass_sampling.hpp"

#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("bandpass sample counts use bounded integer powers of five",
          "[spectra][contracts]") {
    using frost::SizeType;
    using frost::prepare::detail::largest_power_of_five_leq;
    static_assert(largest_power_of_five_leq(8192) == 3125);
    const std::array<SizeType, 10> counts{0,  1,   4,   5,    24,
                                          25, 124, 125, 3124, 3125};
    const std::array<SizeType, 10> expected{0,  1,  1,   5,   5,
                                            25, 25, 125, 625, 3125};
    for (SizeType i = 0; i < counts.size(); ++i) {
        CHECK(largest_power_of_five_leq(counts[i]) == expected[i]);
    }
    const auto limit   = std::numeric_limits<SizeType>::max();
    const auto largest = largest_power_of_five_leq(limit);
    CHECK(largest <= limit);
    CHECK(largest > limit / 5);
    for (SizeType power = 1; power <= largest / 5; power *= 5) {
        CHECK(largest_power_of_five_leq(power) == power);
        CHECK(largest_power_of_five_leq(power * 5 - 1) == power);
    }
}
