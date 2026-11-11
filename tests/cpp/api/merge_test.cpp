#include <algorithm>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <frost/common/policies.hpp>
#include <frost/search/candidate.hpp>
#include <frost/search/merge.hpp>

using frost::SizeType;
using frost::search::Giant;
using frost::search::MergeCandidatesPlan;

namespace {
Giant make_giant(float snr, SizeType members = 1) {
    Giant g;
    g.snr     = snr;
    g.members = members;
    return g;
}
} // namespace

TEST_CASE("MergeCandidatesPlan keeps the highest-SNR candidate per "
          "cluster and sums members",
          "[merge]") {
    const std::vector<Giant> cands = {
        make_giant(5.0F),
        make_giant(9.0F),
        make_giant(3.0F), // cluster 0
        make_giant(7.0F), // cluster 1
    };
    const std::vector<SizeType> labels = {0, 0, 0, 1};

    MergeCandidatesPlan plan;
    const auto merged = plan.execute(cands, labels);

    REQUIRE(merged.size() == 2);
    const auto it0 = std::ranges::find_if(
        merged, [](const Giant& g) { return g.snr == 9.0F; });
    REQUIRE(it0 != merged.end());
    CHECK(it0->members == 3);

    const auto it1 = std::ranges::find_if(
        merged, [](const Giant& g) { return g.snr == 7.0F; });
    REQUIRE(it1 != merged.end());
    CHECK(it1->members == 1);
}

TEST_CASE("MergeCandidatesPlan passes through a single-candidate cluster "
          "unchanged",
          "[merge]") {
    const std::vector<Giant> cands     = {make_giant(4.0F, 2)};
    const std::vector<SizeType> labels = {0};

    MergeCandidatesPlan plan;
    const auto merged = plan.execute(cands, labels);

    REQUIRE(merged.size() == 1);
    CHECK(merged[0].snr == 4.0F);
    CHECK(merged[0].members == 2);
}

TEST_CASE("MergeCandidatesPlan emits clusters in ascending label order",
          "[merge]") {
    const std::vector<Giant> cands = {
        make_giant(1.0F), make_giant(2.0F), make_giant(3.0F),
        make_giant(4.0F), make_giant(5.0F),
    };
    const std::vector<SizeType> labels = {40, 7, 19, 3, 7};

    MergeCandidatesPlan plan;
    const auto merged = plan.execute(cands, labels);

    REQUIRE(merged.size() == 4);
    CHECK(merged[0].snr == 4.0F); // label 3
    CHECK(merged[1].snr == 5.0F); // label 7
    CHECK(merged[1].members == 2);
    CHECK(merged[2].snr == 3.0F); // label 19
    CHECK(merged[3].snr == 1.0F); // label 40
}

TEST_CASE("MergeCandidatesPlan rejects mismatched candidates/labels sizes",
          "[merge]") {
    const std::vector<Giant> cands     = {make_giant(1.0F), make_giant(2.0F)};
    const std::vector<SizeType> labels = {0};

    MergeCandidatesPlan plan;
    CHECK_THROWS_AS(plan.execute(cands, labels), std::invalid_argument);
}
