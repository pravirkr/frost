#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <frost/utils/stats.hpp>

using frost::SizeType;

TEST_CASE(
    "matched filters reject invalid geometry before changing state or output",
    "[stats][contracts]") {
    const std::array<float, 4> input{1, 2, 3, 4};
    std::array<float, 3> output{99, 99, 99};
    frost::stats::MatchedFilterPlan plan;
    CHECK_THROWS_AS(plan.execute(output, 1), std::logic_error);
    plan.prepare(input, 2);
    CHECK_THROWS_AS(plan.prepare(input, 0), std::invalid_argument);
    CHECK_THROWS_AS(plan.prepare(input, 5), std::invalid_argument);
    CHECK_THROWS_AS(plan.prepare(nullptr, 4, 2), std::invalid_argument);
    CHECK_THROWS_AS(plan.prepare(input.data(), 0, 1), std::invalid_argument);
    CHECK_THROWS_AS(
        plan.prepare(input.data(), std::numeric_limits<SizeType>::max(), 1),
        std::invalid_argument);
    CHECK(plan.prefix().count == 4);
    CHECK(plan.prefix().max_width == 2);
    CHECK_THROWS_AS(plan.execute(output, 0), std::invalid_argument);
    CHECK_THROWS_AS(plan.execute(output, 3), std::invalid_argument);
    CHECK_THROWS_AS(plan.execute(output, 2, 0), std::invalid_argument);
    CHECK_THROWS_AS(plan.execute(nullptr, 2), std::invalid_argument);
    CHECK_THROWS_AS(plan.execute(std::span(output).first(2), 2),
                    std::invalid_argument);
    CHECK((output == std::array<float, 3>{99, 99, 99}));
    plan.execute(output, 2);
    CHECK((output == std::array<float, 3>{3, 5, 7}));
    output.fill(99);
    plan.execute(std::span(output).first(2), 2, 2);
    CHECK((output == std::array<float, 3>{3, 7, 99}));
}

TEST_CASE("matched filter spans preserve centered sums at every valid width",
          "[stats][contracts]") {
    const std::array<float, 9> input{0, 1, 2, 8, 4, 0, 6, 2, 1};
    frost::stats::MatchedFilterPlan plan;
    plan.prepare(input, 6);
    for (SizeType width = 1; width <= 6; ++width) {
        for (SizeType stride : {1U, 2U, 9U}) {
            std::array<float, 5> output{};
            output.fill(-100);
            plan.execute(std::span(output).first(4), width, stride);
            SizeType written = 0;
            for (SizeType i = 0; i < 4; i += stride) {
                const auto center = i + 3;
                const auto begin  = center - width / 2;
                float sum         = 0;
                for (SizeType j = begin; j < begin + width; ++j)
                    sum += input[j];
                CHECK(output[written++] == sum);
            }
            for (; written < output.size(); ++written)
                CHECK(output[written] == -100);
        }
    }
}

TEST_CASE("running filters clip short input and use the requested window width",
          "[stats][contracts]") {
    for (SizeType count = 1; count <= 9; ++count) {
        std::vector<float> input(count), output(count);
        for (SizeType i = 0; i < count; ++i)
            input[i] = static_cast<float>((7 * i + 3) % 11);
        for (const auto method : {"mean", "median"}) {
            for (SizeType width = 1; width <= 11; ++width) {
                if (std::string_view(method) == "median" && width % 2 == 0)
                    continue;
                frost::stats::running_filter(input.data(), count, output.data(),
                                             count, width, method);
                for (SizeType i = 0; i < count; ++i) {
                    const auto begin = i > width / 2 ? i - width / 2 : 0;
                    const auto end = i + std::min(width - width / 2, count - i);
                    std::vector<float> window(input.begin() + begin,
                                              input.begin() + end);
                    float expected = 0;
                    if (std::string_view(method) == "mean") {
                        for (const auto value : window)
                            expected += value;
                        expected /= static_cast<float>(window.size());
                    } else {
                        std::sort(window.begin(), window.end());
                        expected = window[window.size() / 2];
                        if ((width == 3 || width == 5) &&
                            window.size() % 2 == 0) {
                            expected = 0.5F * expected +
                                       0.5F * window[window.size() / 2 - 1];
                        }
                    }
                    CHECK(output[i] == Catch::Approx(expected));
                }
            }
        }
    }
}

TEST_CASE(
    "running filters support overlapping storage and bounded huge windows",
    "[stats][contracts]") {
    std::vector<float> values{9, 1, 8, 2, 7};
    frost::stats::running_filter(values.data(), values.size(), values.data(),
                                 values.size(), 3);
    CHECK((values == std::vector<float>{5, 8, 2, 7, 4.5F}));
    values = {9, 1, 8, 2, 7};
    frost::stats::running_filter(values.data(), values.size(),
                                 values.data() + 1, 4, 3);
    CHECK((values == std::vector<float>{9, 5, 8, 2, 7}));
    std::array<float, 3> output{};
    const std::array<float, 3> input{1, 3, 8};
    const auto huge = std::numeric_limits<SizeType>::max();
    frost::stats::running_filter(input.data(), 3, output.data(), 3, huge);
    for (const auto value : output)
        CHECK(value == 3);
    frost::stats::running_filter(input.data(), 3, output.data(), 3, huge,
                                 "mean");
    for (const auto value : output)
        CHECK(value == 4);
}

TEST_CASE("running filters reject malformed buffers and windows before writing",
          "[stats][contracts]") {
    const std::array<float, 2> input{1, 2};
    std::array<float, 3> output{99, 99, 99};
    for (const auto method : {"mean", "median"}) {
        CHECK_THROWS_AS(frost::stats::running_filter(
                            input.data(), 2, output.data(), 3, 3, method),
                        std::invalid_argument);
        CHECK_THROWS_AS(frost::stats::running_filter(
                            input.data(), 2, output.data(), 2, 0, method),
                        std::invalid_argument);
        CHECK_THROWS_AS(frost::stats::running_filter(nullptr, 2, output.data(),
                                                     2, 3, method),
                        std::invalid_argument);
        CHECK_THROWS_AS(frost::stats::running_filter(input.data(), 2, nullptr,
                                                     2, 3, method),
                        std::invalid_argument);
        CHECK_NOTHROW(
            frost::stats::running_filter(nullptr, 0, nullptr, 0, 1, method));
    }
    CHECK_THROWS_AS(
        frost::stats::running_filter(input.data(), 2, output.data(), 2, 2),
        std::invalid_argument);
    CHECK_THROWS_AS(frost::stats::running_filter(input.data(), 2, output.data(),
                                                 2, 3, "typo"),
                    std::invalid_argument);
    CHECK((output == std::array<float, 3>{99, 99, 99}));
    frost::stats::EstimateScalePlan plan("std");
    CHECK_THROWS_AS(plan.execute(nullptr, 2), std::invalid_argument);
}

TEST_CASE("standard deviation avoids float-square overflow and underflow",
          "[stats][stability]") {
    frost::stats::EstimateScalePlan plan("std");
    for (const float amplitude : {1.0e20F, 1.0e-20F}) {
        const std::array<float, 4> input{-amplitude, amplitude, -amplitude,
                                         amplitude};
        CHECK(plan.execute(input.data(), input.size()) ==
              Catch::Approx(amplitude));
    }
}

TEST_CASE("baseline smoothing bounds integer arithmetic before accessing input",
          "[stats][contracts]") {
    frost::stats::BaselineFilterPlan plan;
    std::array<float, 4> input{1, 2, 3, 4};
    const auto huge = std::numeric_limits<SizeType>::max();
    CHECK_NOTHROW(plan.execute(input.data(), input.size(), huge));
    CHECK_NOTHROW(plan.execute(input.data(), input.size(), huge / 2 + 1));
    CHECK((input == std::array<float, 4>{1, 2, 3, 4}));
    CHECK_NOTHROW(plan.execute(nullptr, 0, 1));
    CHECK_THROWS_AS(plan.execute(nullptr, 4, 1), std::invalid_argument);
    CHECK_THROWS_AS(plan.execute(input.data(), 4, 0), std::invalid_argument);
    CHECK_THROWS_AS(plan.execute(input.data(), huge, 1), std::invalid_argument);
}
