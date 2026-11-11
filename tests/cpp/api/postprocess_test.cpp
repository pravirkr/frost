#include <bit>
#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <frost/search/postprocess.hpp>

namespace {
dmt::RawCandidate peak() {
    dmt::RawCandidate raw;
    raw.peak_sample  = 100;
    raw.begin_sample = 99;
    raw.end_sample   = 101;
    raw.snr          = 10.0F;
    raw.dm           = 20.0F;
    raw.width        = 4;
    raw.chan_begin   = 1;
    raw.chan_end     = 5;
    raw.f_low_mhz    = 1400.0F;
    raw.f_high_mhz   = 1404.0F;
    return raw;
}
dmt::CandidateResultView view(const std::vector<dmt::RawCandidate>& raw) {
    dmt::CandidateResultView out;
    out.candidates = raw;
    return out;
}
} // namespace
TEST_CASE(
    "candidate postprocessor consumes a borrowed view and maps channel order",
    "[postprocess]") {
    frost::config::SearchConfig cfg;
    cfg.cluster = false;
    for (const double df : {-1.0, 1.0}) {
        frost::search::CandidatePostprocessPlan plan(cfg, {8, 0.001, df});
        std::vector<dmt::RawCandidate> raw{peak()};
        raw[0].end_sample = 1000;
        auto result       = plan.execute(view(raw), 200, 200, false, 200);
        REQUIRE(result.candidates.size() == 1);
        CHECK(result.candidates[0].samp_idx == 100);
        CHECK(result.candidates[0].end == 199);
        CHECK(result.candidates[0].chan_begin == (df < 0 ? 3 : 1));
        CHECK(result.candidates[0].chan_end == (df < 0 ? 6 : 4));
        raw.clear();
        CHECK(result.candidates[0].snr == 10.0F);
        CHECK(plan.execute({}, 200, 0, true, 200).candidates.empty());
    }
}
TEST_CASE("candidate postprocessor retains recent peaks and drains at EOF",
          "[postprocess]") {
    frost::config::SearchConfig cfg;
    cfg.boxcar_max        = 16;
    cfg.cand_sep_dm_smear = 0.0F;
    frost::search::CandidatePostprocessPlan plan(cfg, {8, 0.001, 1.0});
    std::vector<dmt::RawCandidate> raw{peak()};
    auto first = plan.execute(view(raw), 110, 110, false, 100);
    CHECK(first.candidates.empty());
    CHECK(first.pending_candidates == 1);
    raw[0].snr        = 20.0F;
    raw[0].width      = 8;
    raw[0].filter_idx = 1;
    raw[0].end_sample = 111;
    auto last         = plan.execute(view(raw), 120, 10, true, 120);
    REQUIRE(last.candidates.size() == 1);
    CHECK(last.candidates[0].snr == 20.0F);
    CHECK(last.candidates[0].members == 2);
    CHECK(last.pending_candidates == 0);
    CHECK(plan.execute({}, 120, 0, true, 120).candidates.empty());
}

TEST_CASE(
    "postprocessor bounds pending storage and reuses its reserved buffers",
    "[postprocess][memory]") {
    frost::config::SearchConfig cfg;
    cfg.boxcar_max             = 16;
    cfg.max_candidates         = 4;
    cfg.max_pending_candidates = 1;
    cfg.cand_sep_dm            = 0;
    cfg.cand_sep_dm_smear      = 0.0F;
    frost::search::CandidatePostprocessPlan plan(cfg, {8, 0.001, 1.0});
    const auto reserved = plan.memory_usage();
    CHECK(reserved <=
          frost::search::CandidatePostprocessPlan::estimate_memory(cfg));
    std::vector<dmt::RawCandidate> raw{peak(), peak()};
    raw[1].dm_idx = 100;
    CHECK_THROWS_WITH(plan.execute(view(raw), 110, 110, false, 100),
                      "Pending candidate capacity exceeded; increase "
                      "max_pending_candidates or reduce candidate production");
    CHECK(plan.memory_usage() == reserved);
    frost::search::CandidatePostprocessPlan bounded(cfg, {8, 0.001, 1.0});
    raw.resize(1);
    bounded.execute(view(raw), 110, 110, false, 110);
    bounded.execute({}, 200, 90, true, 200);
    CHECK(bounded.memory_usage() == reserved);
}

TEST_CASE(
    "postprocessor retains weaker cluster members that bridge later gulps",
    "[postprocess][streaming][robust]") {
    frost::config::SearchConfig cfg;
    cfg.boxcar_max             = 16;
    cfg.cand_sep_time          = 1;
    cfg.cand_sep_dm_smear      = 0;
    cfg.max_pending_candidates = 4;
    for (const frost::SizeType read : {120U, 140U}) {
        // At read=140 the winning excursion is already older than the
        // watermark, but its weaker member can still meet a future peak.
        frost::search::CandidatePostprocessPlan plan(cfg, {8, 0.001, 1.0});
        auto strong         = peak();
        strong.width        = 16;
        strong.snr          = 20;
        auto bridge         = strong;
        bridge.snr          = 10;
        bridge.peak_sample  = 115;
        bridge.begin_sample = 114;
        bridge.end_sample   = 116;
        const std::vector<dmt::RawCandidate> first{strong, bridge};
        const auto pending = plan.execute(view(first), read, read, false, read);
        CHECK(pending.candidates.empty());
        CHECK(pending.pending_candidates == 2);
        auto later         = bridge;
        later.snr          = 9;
        later.peak_sample  = 130;
        later.begin_sample = 129;
        later.end_sample   = 131;
        // The engine may close this excursion in a later call even though
        // its peak refers to previously read input.
        const std::vector<dmt::RawCandidate> last{later};
        const auto result =
            plan.execute(view(last), 200, 200 - read, true, 200);
        REQUIRE(result.candidates.size() == 1);
        CHECK(result.candidates[0].samp_idx == 100);
        CHECK(result.candidates[0].snr == 20);
        CHECK(result.candidates[0].members == 3);
        CHECK(result.candidates[0].begin == 99);
        CHECK(result.candidates[0].end == 101);
        CHECK(result.pending_candidates == 0);
    }
}

TEST_CASE(
    "postprocessor carries members of seeded DM-wing groups independently",
    "[postprocess][streaming][robust]") {
    frost::config::SearchConfig cfg;
    cfg.boxcar_max             = 16;
    cfg.cand_sep_time          = 1;
    cfg.cand_sep_dm_smear      = 3;
    cfg.max_pending_candidates = 4;
    frost::search::CandidatePostprocessPlan plan(cfg, {8, 0.001, 1.0});
    auto strong         = peak();
    strong.f_low_mhz    = 1000;
    strong.f_high_mhz   = 1500;
    strong.dm           = 30;
    strong.dm_idx       = 10;
    strong.snr          = 50;
    auto bridge         = strong;
    bridge.dm           = 20;
    bridge.dm_idx       = 0;
    bridge.snr          = 10;
    bridge.peak_sample  = 104;
    bridge.begin_sample = 103;
    bridge.end_sample   = 105;
    auto old            = strong;
    old.peak_sample = old.begin_sample = old.end_sample = 0;
    old.snr                                             = 8;
    old.dm_idx                                          = 100;
    old.dm                                              = 100;
    const std::vector<dmt::RawCandidate> first{strong, bridge, old};
    const auto pending = plan.execute(view(first), 136, 136, false, 136);
    REQUIRE(pending.candidates.size() == 1);
    CHECK(pending.candidates[0].samp_idx == 0);
    CHECK(pending.pending_candidates == 2);
    auto later         = bridge;
    later.snr          = 9;
    later.peak_sample  = 108;
    later.begin_sample = 107;
    later.end_sample   = 109;
    const std::vector<dmt::RawCandidate> last{later};
    const auto result = plan.execute(view(last), 200, 64, true, 200);
    REQUIRE(result.candidates.size() == 1);
    CHECK(result.candidates[0].samp_idx == 100);
    CHECK(result.candidates[0].dm == 30);
    CHECK(result.candidates[0].snr == 50);
    CHECK(result.candidates[0].members == 3);
    CHECK(result.pending_candidates == 0);
}

TEST_CASE(
    "pending capacity counts retained cluster members rather than winners",
    "[postprocess][streaming][memory]") {
    frost::config::SearchConfig cfg;
    cfg.boxcar_max             = 16;
    cfg.cand_sep_dm_smear      = 0;
    cfg.max_pending_candidates = 1;
    frost::search::CandidatePostprocessPlan plan(cfg, {8, 0.001, 1.0});
    auto first         = peak();
    auto second        = first;
    second.peak_sample = 101;
    second.end_sample  = 102;
    const std::vector<dmt::RawCandidate> raw{first, second};
    CHECK_THROWS_WITH(plan.execute(view(raw), 110, 110, false, 100),
                      "Pending candidate capacity exceeded; increase "
                      "max_pending_candidates or reduce candidate production");
}

TEST_CASE("postprocessor rejects nonfinite or degenerate observing geometry",
          "[postprocess][contracts]") {
    const frost::config::SearchConfig cfg;
    const double nan = std::bit_cast<double>(std::uint64_t{0x7ff8000000000000});
    const double infinity =
        std::bit_cast<double>(std::uint64_t{0x7ff0000000000000});
    for (const auto invalid : {0.0, -1.0, infinity, nan}) {
        CHECK_THROWS_AS(
            (frost::search::CandidatePostprocessPlan(cfg, {8, invalid, 1})),
            std::invalid_argument);
    }
    for (const auto invalid : {0.0, infinity, nan}) {
        CHECK_THROWS_AS(
            (frost::search::CandidatePostprocessPlan(cfg, {8, 0.001, invalid})),
            std::invalid_argument);
    }
}

TEST_CASE("postprocessor rejects malformed excursions and foreign beams",
          "[postprocess][contracts]") {
    frost::config::SearchConfig cfg;
    cfg.boxcar_max = 16;
    cfg.cluster    = false;
    frost::search::CandidatePostprocessPlan plan(cfg, {8, 0.001, 1});
    auto bad = peak();
    SECTION("foreign beam") { bad.beam_idx = 1; }
    SECTION("zero width") { bad.width = 0; }
    SECTION("width beyond planned maximum") { bad.width = 17; }
    SECTION("begin after peak") { bad.begin_sample = 101; }
    SECTION("end before peak") { bad.end_sample = 99; }
    const std::vector<dmt::RawCandidate> invalid{bad};
    CHECK_THROWS_AS(plan.execute(view(invalid), 200, 200, false, 200),
                    std::invalid_argument);
    const std::vector<dmt::RawCandidate> valid{peak()};
    const auto result = plan.execute(view(valid), 200, 200, false, 200);
    REQUIRE(result.candidates.size() == 1);
    CHECK(result.candidates[0].samp_idx == 100);
}

TEST_CASE("postprocessor rate circuit breakers are independent per plane",
          "[postprocess][rate_limiter]") {
    frost::config::SearchConfig cfg;
    cfg.cluster        = false;
    cfg.max_giant_rate = 60; // One row per second, then retain the trip row.
    cfg.subband_levels = 1;
    frost::search::CandidatePostprocessPlan plan(cfg, {8, 0.001, 1});
    std::vector<dmt::RawCandidate> raw(4, peak());
    raw[1].peak_sample = raw[1].end_sample = 110;
    raw[2].peak_sample = raw[2].end_sample = 120;
    raw[3].subband_level                   = 1;
    raw[3].chan_end                        = 3;
    const auto result = plan.execute(view(raw), 1000, 1000, true, 1000);
    REQUIRE(result.candidates.size() == 3);
    CHECK(result.candidates[0].samp_idx == 100);
    CHECK(result.candidates[1].samp_idx == 110);
    CHECK(result.candidates[2].plane != result.candidates[0].plane);
}

TEST_CASE("postprocessor releases groups at the engine candidate frontier",
          "[postprocess][streaming]") {
    frost::config::SearchConfig cfg;
    cfg.boxcar_max        = 16;
    cfg.cand_sep_time     = 1;
    cfg.cand_sep_dm_smear = 0;
    frost::search::CandidatePostprocessPlan plan(cfg, {8, 0.001, 1.0});
    const std::vector<dmt::RawCandidate> raw{peak()};
    // Input has advanced far past the peak, but an excursion that began
    // at sample 90 is still open: a later peak could join this group.
    auto held = plan.execute(view(raw), 100'000, 100'000, false, 90);
    CHECK(held.candidates.empty());
    CHECK(held.pending_candidates == 1);
    // Still within (cand_sep_time + 1) * boxcar_max = 32 of the frontier.
    held = plan.execute({}, 100'000, 0, false, 101 + 32);
    CHECK(held.candidates.empty());
    CHECK(held.pending_candidates == 1);
    // Once the frontier passes that reach the group is final.
    const auto released = plan.execute({}, 100'000, 0, false, 101 + 33);
    REQUIRE(released.candidates.size() == 1);
    CHECK(released.candidates[0].samp_idx == 100);
    CHECK(released.pending_candidates == 0);
}
