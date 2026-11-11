#include <bit>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include <frost/common/numeric.hpp>

TEST_CASE("float classification covers IEEE boundary representations",
          "[numeric][robust]") {
    static_assert(frost::is_finite(0.0F));
    static_assert(frost::is_positive_finite(1.0F));
    static_assert(!frost::is_finite(std::numeric_limits<float>::infinity()));
    for (const std::uint32_t bits : {0x7F800000U, 0xFF800000U, 0x7FC00000U,
                                     0xFFC00000U, 0x7F800001U, 0xFF800001U}) {
        CAPTURE(bits);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        CHECK_FALSE(frost::is_finite_bits(bits));
        CHECK_FALSE(frost::is_finite(value));
        CHECK_FALSE(frost::is_positive_finite(value));
    }
    for (const std::uint32_t bits :
         {0U, 0x80000000U, 1U, 0x80000001U, 0x00800000U, 0x80800000U,
          0x7F7FFFFFU, 0xFF7FFFFFU}) {
        CAPTURE(bits);
        CHECK(frost::is_finite(std::bit_cast<float>(bits)));
    }
}

TEST_CASE("double classification covers IEEE boundary representations",
          "[numeric][robust]") {
    static_assert(frost::is_finite(0.0));
    static_assert(frost::is_positive_finite(1.0));
    static_assert(!frost::is_finite(std::numeric_limits<double>::infinity()));
    for (const std::uint64_t bits :
         {0x7FF0000000000000ULL, 0xFFF0000000000000ULL, 0x7FF8000000000000ULL,
          0xFFF8000000000000ULL, 0x7FF0000000000001ULL,
          0xFFF0000000000001ULL}) {
        CAPTURE(bits);
        double value;
        std::memcpy(&value, &bits, sizeof(value));
        CHECK_FALSE(frost::is_finite_bits(bits));
        CHECK_FALSE(frost::is_finite(value));
        CHECK_FALSE(frost::is_positive_finite(value));
    }
    for (const std::uint64_t bits :
         {0ULL, 0x8000000000000000ULL, 1ULL, 0x8000000000000001ULL,
          0x0010000000000000ULL, 0x8010000000000000ULL, 0x7FEFFFFFFFFFFFFFULL,
          0xFFEFFFFFFFFFFFFFULL}) {
        CAPTURE(bits);
        CHECK(frost::is_finite(std::bit_cast<double>(bits)));
    }
}
