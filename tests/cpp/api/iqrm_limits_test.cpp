#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <frost/prepare/rfi.hpp>

TEST_CASE("IQRM stops after lagged comparisons saturate at the channel edges",
          "[rfi][iqrm][contracts]") {
    using frost::SizeType;
    using frost::prepare::rfi::iqrm_mask;
    for (const SizeType count : {1U, 2U, 3U, 8U, 32U, 97U}) {
        std::vector<float> values(count);
        std::vector<uint8_t> ignore(count);
        for (SizeType i = 0; i < count; ++i)
            values[i] = static_cast<float>((i * 17 + 3) % 11);
        values[count / 2] += 100;
        for (const bool ignored_edge : {false, true}) {
            ignore.back() = static_cast<uint8_t>(ignored_edge);
            // Every lag >= count-1 makes the same edge-clamped comparisons;
            // duplicate votes cannot change the result. Two channel counts
            // include the first such geometric lag without size overflow.
            const auto expected = iqrm_mask(values, 2 * count, 3, ignore);
            CHECK(iqrm_mask(values, std::numeric_limits<SizeType>::max(), 3,
                            ignore) == expected);
        }
    }
}
