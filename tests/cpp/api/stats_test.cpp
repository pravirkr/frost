#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <frost/utils/stats.hpp>

TEST_CASE("estimate_scale returns positive std for simple data", "[stats]") {
    const float data[] = {1.0F, 2.0F, 3.0F, 4.0F, 5.0F};
    const float scale  = frost::stats::estimate_scale(data, 5, "std");
    REQUIRE(scale > 0.0F);
}

TEST_CASE("standard deviation retains small noise above a large DC level",
          "[stats][stability]") {
    constexpr float kLevel = 100'000'000.0F;
    std::vector<float> input(4096, kLevel);
    frost::stats::EstimateScalePlan plan("std");
    CHECK(plan.execute(input.data(), input.size()) == 0.0F);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] += i % 2 ? 16.0F : -16.0F;
    CHECK(plan.execute(input.data(), input.size()) == Catch::Approx(16.0F));
    std::reverse(input.begin(), input.end());
    CHECK(plan.execute(input.data(), input.size()) == Catch::Approx(16.0F));
}

TEST_CASE("MAD uses the exact median absolute value for every input length",
          "[stats][mad]") {
    constexpr float kMadNorm = 0.6744897501960817F;
    frost::stats::EstimateScalePlan plan("mad");
    // Reuse the plan across growing/shrinking inputs, including all median
    // parity cases and lengths around powers of five.
    std::mt19937 rng(374);
    for (const std::size_t count :
         {1U, 2U, 3U, 4U, 5U, 6U, 24U, 25U, 26U, 124U, 125U, 126U, 8192U, 3U}) {
        std::vector<float> input(count);
        for (std::size_t i = 0; i < count; ++i) {
            input[i] = static_cast<float>(i + 1) * (i % 2 ? -1.0F : 1.0F);
        }
        std::shuffle(input.begin(), input.end(), rng);
        const auto original         = input;
        const float expected_median = (static_cast<float>(count) + 1) / 2;
        INFO("count=" << count);
        CHECK(plan.execute(input.data(), count) ==
              Catch::Approx(expected_median / kMadNorm));
        CHECK(input == original);
    }
}

TEST_CASE("MAD includes a gulp's late majority and is invariant to ordering",
          "[stats][mad]") {
    constexpr float kMadNorm = 0.6744897501960817F;
    std::vector<float> input(8192, 8.0F);
    std::fill_n(input.begin(), 3125, -1.0F);
    frost::stats::EstimateScalePlan plan("mad");
    CHECK(plan.execute(input.data(), input.size()) ==
          Catch::Approx(8 / kMadNorm));
    std::reverse(input.begin(), input.end());
    CHECK(plan.execute(input.data(), input.size()) ==
          Catch::Approx(8 / kMadNorm));
    std::mt19937 rng(375);
    std::shuffle(input.begin(), input.end(), rng);
    CHECK(plan.execute(input.data(), input.size()) ==
          Catch::Approx(8 / kMadNorm));
}

TEST_CASE("seven-point running median uses the upper median at edges",
          "[stats]") {
    const std::vector<float> input    = {9, 1, 8, 2, 7, 3, 6, 4, 5};
    const std::vector<float> expected = {8, 7, 7, 6, 4, 5, 5, 5, 5};
    std::vector<float> output(input.size());
    frost::stats::running_filter(input.data(), input.size(), output.data(),
                                 output.size(), 7, "median");
    CHECK(output == expected);
}

TEST_CASE("BaselineFilterPlan tracks a slow drift right up to both edges",
          "[stats]") {
    // 31457 / (2 * 1953) gives 16 knots over 5^4 * 16 = 10000 resampled
    // points: an exact power-of-five case where float log/pow arithmetic
    // under -ffast-math would truncate 5^4 to 624.
    constexpr std::size_t kCount  = 31457;
    constexpr std::size_t kRadius = 1953;
    constexpr float kOffset       = 25000.0F;
    constexpr float kSigma        = 68.0F;

    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0F, kSigma);
    std::vector<float> row(kCount);
    // 20 sigma of linear drift across the row, which the piecewise-linear
    // baseline should follow exactly.
    const float slope = 20.0F * kSigma / static_cast<float>(kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
        row[i] = kOffset + (slope * static_cast<float>(i)) + noise(rng);
    }

    frost::stats::BaselineFilterPlan plan;
    plan.execute(row.data(), kCount, kRadius);

    // Mean residual over widest-boxcar-sized windows. Knot noise (each
    // knot is a ~2000-sample median) plus placement error is < 0.15 sigma.
    constexpr std::size_t kEdge = 1024;
    const auto mean             = [&](std::size_t begin) {
        double sum = 0.0;
        for (std::size_t i = begin; i < begin + kEdge; ++i) {
            sum += row[i];
        }
        return sum / static_cast<double>(kEdge);
    };
    CHECK(std::abs(mean(0)) < 0.25 * kSigma);
    CHECK(std::abs(mean(kCount - kEdge)) < 0.25 * kSigma);
    CHECK(std::abs(mean(kCount / 2)) < 0.25 * kSigma);
}
