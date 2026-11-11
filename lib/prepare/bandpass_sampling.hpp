#pragma once

#include <frost/common/policies.hpp>

namespace frost::prepare::detail {

/** Largest power of five not exceeding count; zero has no such power. */
constexpr SizeType largest_power_of_five_leq(SizeType count) noexcept {
    if (count == 0)
        return 0;
    SizeType power = 1;
    while (power <= count / 5)
        power *= 5;
    return power;
}

} // namespace frost::prepare::detail
