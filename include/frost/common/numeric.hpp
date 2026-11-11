#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace frost {

namespace detail {
// Opaque integer-only boundary: later FP uses must not let the optimizer
// infer that the stored representation was finite before validation.
bool finite_float_representation(const void* data) noexcept;
bool finite_double_representation(const void* data) noexcept;
} // namespace detail

// Inspect representations before any floating-point load/arithmetic.
// Passing an already-loaded NaN/Inf by value is incompatible with fast-math.
[[nodiscard]] constexpr bool is_finite_bits(std::uint32_t bits) noexcept {
    return (bits & 0x7F800000U) != 0x7F800000U;
}

[[nodiscard]] constexpr bool is_finite_bits(std::uint64_t bits) noexcept {
    return (bits & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL;
}

[[nodiscard]] constexpr bool is_finite(const float& x) noexcept {
    if (std::is_constant_evaluated())
        return is_finite_bits(std::bit_cast<std::uint32_t>(x));
    return detail::finite_float_representation(&x);
}

[[nodiscard]] constexpr bool is_finite(const double& x) noexcept {
    if (std::is_constant_evaluated())
        return is_finite_bits(std::bit_cast<std::uint64_t>(x));
    return detail::finite_double_representation(&x);
}

/** Checks the stored representation before comparing the finite value. */
template <typename T>
[[nodiscard]] constexpr bool is_positive_finite(const T& x) {
    return is_finite(x) && x > T{0};
}

} // namespace frost
