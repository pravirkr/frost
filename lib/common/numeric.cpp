#include <cstdint>
#include <cstring>

#include <frost/common/numeric.hpp>

namespace frost::detail {

// Keep the validation boundary intact under IPO as well as ordinary builds.
[[gnu::noinline]] bool finite_float_representation(const void* data) noexcept {
    std::uint32_t bits;
    std::memcpy(&bits, data, sizeof(bits));
    return is_finite_bits(bits);
}

[[gnu::noinline]] bool finite_double_representation(const void* data) noexcept {
    std::uint64_t bits;
    std::memcpy(&bits, data, sizeof(bits));
    return is_finite_bits(bits);
}

} // namespace frost::detail
