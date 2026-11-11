// Input-validation and numerical-robustness tests. Each case asserts the
// intended behavior for an edge case: degenerate geometry, overflow, or
// configuration that could otherwise never terminate.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <dmt/common/boxcar_plans.hpp>

#include <frost/common/numeric.hpp>
#include <frost/config/config.hpp>
#include <frost/dedisp/dedisp.hpp>
#include <frost/search/candidate.hpp>
#include <frost/search/subband_dedup.hpp>
#include <frost/utils/stats.hpp>

#include "support/temp_dir.hpp"

using frost::SizeType;
using frost::config::PipelineConfig;

namespace {

PipelineConfig load_toml(const std::string& text) {
    const auto dir  = frost::test::unique_temp_dir("robust_cfg");
    const auto path = (dir / "c.toml").string();
    {
        std::ofstream out(path);
        out << text;
    }
    return PipelineConfig::from_toml(path);
}

frost::dedisp::DedispPlanConfig plan_config() {
    frost::dedisp::DedispPlanConfig dd;
    dd.nchans         = 64;
    dd.dt             = 1.0e-3F;
    dd.f0             = 1500.0F;
    dd.df             = -2.0F;
    dd.dm_min         = 0.0F;
    dd.dm_max         = 100.0F;
    dd.dm_pulse_width = 40.0F;
    dd.dm_tol         = 1.25F;
    return dd;
}

} // namespace

TEST_CASE("raw IEEE classification rejects NaN and infinity under fast-math",
          "[numeric]") {
    for (const std::uint32_t bits : {0x7FC00000U, 0x7F800000U, 0xFF800000U}) {
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        CHECK_FALSE(frost::is_finite_bits(bits));
        CHECK_FALSE(frost::is_finite(value));
    }
    CHECK(frost::is_finite(0.0F));
    CHECK(frost::is_finite(-1.5e30F));
    CHECK_FALSE(frost::is_positive_finite(0.0F));
    CHECK_FALSE(frost::is_positive_finite(-1.0F));
    CHECK(frost::is_positive_finite(1.0e-3));
}

TEST_CASE("from_toml rejects unknown keys, naming the dotted path",
          "[config][robust]") {
    CHECK_THROWS_WITH(load_toml("nsamps_guls = 4096\n"),
                      Catch::Matchers::ContainsSubstring("nsamps_guls"));
    CHECK_THROWS_WITH(load_toml("[search]\nboxcar_mx = 64\n"),
                      Catch::Matchers::ContainsSubstring("search.boxcar_mx"));
    CHECK_THROWS_WITH(load_toml("[rfi.broad]\nthreshld = 3.0\n"),
                      Catch::Matchers::ContainsSubstring("threshld"));
    CHECK_THROWS_AS(load_toml("[bogus]\nx = 1\n"), std::exception);
}

TEST_CASE("from_toml rejects negative sizes instead of wrapping them",
          "[config][robust]") {
    CHECK_THROWS(load_toml("nsamps_gulp = -1\n"));
    CHECK_THROWS(load_toml("[search]\nboxcar_max = -4\n"));
}

TEST_CASE("PipelineConfig::validate accepts the defaults", "[config][robust]") {
    PipelineConfig cfg;
    cfg.nsamps_gulp = 1 << 16;
    CHECK_NOTHROW(cfg.validate());
}

TEST_CASE("PipelineConfig::validate rejects degenerate settings",
          "[config][robust]") {
    const auto good = [] {
        PipelineConfig cfg;
        cfg.nsamps_gulp = 1 << 16;
        return cfg;
    };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    auto cfg          = good();
    cfg.dedisp.dm_tol = 1.0F; // the DM list would never advance
    CHECK_THROWS_AS(cfg.validate(), std::invalid_argument);

    cfg               = good();
    cfg.dedisp.dm_tol = 0.5F;
    CHECK_THROWS_AS(cfg.validate(), std::invalid_argument);

    cfg               = good();
    cfg.dedisp.dm_max = inf;
    CHECK_THROWS_AS(cfg.validate(), std::invalid_argument);

    cfg               = good();
    cfg.dedisp.dm_min = nan;
    CHECK_THROWS_AS(cfg.validate(), std::invalid_argument);

    cfg               = good();
    cfg.dedisp.dm_min = 10.0F;
    cfg.dedisp.dm_max = 5.0F;
    CHECK_THROWS_AS(cfg.validate(), std::invalid_argument);

    cfg                   = good();
    cfg.search.boxcar_max = 0;
    CHECK_THROWS_AS(cfg.validate(), std::invalid_argument);

    cfg                   = good();
    cfg.search.boxcar_max = cfg.nsamps_gulp;
    CHECK_THROWS_AS(cfg.validate(), std::invalid_argument);

    cfg             = good();
    cfg.search.wtsp = 0.5F;
    CHECK_THROWS_AS(cfg.validate(), std::invalid_argument);

    cfg                 = good();
    cfg.baseline_length = 0.0F;
    CHECK_THROWS_AS(cfg.validate(), std::invalid_argument);

    cfg             = good();
    cfg.nsamps_gulp = 0;
    CHECK_THROWS_AS(cfg.validate(), std::invalid_argument);

    cfg                      = good();
    cfg.search.detect_thresh = -1.0F;
    CHECK_THROWS_AS(cfg.validate(), std::invalid_argument);

    cfg                          = good();
    cfg.search.cand_sep_dm_smear = -1.0F;
    CHECK_THROWS_AS(cfg.validate(), std::invalid_argument);
    cfg.search.cand_sep_dm_smear = nan;
    CHECK_THROWS_AS(cfg.validate(), std::invalid_argument);
    cfg.search.cand_sep_dm_smear = 0.0F; // trial count only is allowed
    CHECK_NOTHROW(cfg.validate());
}

TEST_CASE("generate_dm_list rejects a non-advancing or invalid tolerance",
          "[dedisp][robust]") {
    auto dd = plan_config();
    CHECK_NOTHROW(frost::dedisp::generate_dm_list(dd));

    dd.dm_tol = 1.0F; // would never terminate the DM list
    CHECK_THROWS_AS(frost::dedisp::generate_dm_list(dd), std::invalid_argument);
    dd.dm_tol = 0.9F;
    CHECK_THROWS_AS(frost::dedisp::generate_dm_list(dd), std::invalid_argument);
    dd.dm_tol = std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS_AS(frost::dedisp::generate_dm_list(dd), std::invalid_argument);

    dd = plan_config();
    // Inject the external representation without first loading infinity
    // through a floating-point value under finite-only assumptions.
    const std::uint32_t infinity_bits = 0x7F800000U;
    std::memcpy(&dd.dm_max, &infinity_bits, sizeof(dd.dm_max));
    REQUIRE_FALSE(frost::is_finite(dd.dm_max));
    CHECK_THROWS_AS(frost::dedisp::generate_dm_list(dd), std::invalid_argument);
}

TEST_CASE("baseline_smooth_radius works in double and keeps >= 2 knots",
          "[stats][robust]") {
    // float(1e-3) is 1.0000000475e-3: a float-only formula gives 999 samples
    // for a 2 s window.
    CHECK(frost::stats::baseline_smooth_radius(2.0F, 1.0e-3, 1, 1 << 20) ==
          1000);
    CHECK(frost::stats::baseline_smooth_radius(2.0F, 1.0e-3, 4, 1 << 20) ==
          250);
    // A short series: radius is capped at count / 2 so there are two knots.
    CHECK(frost::stats::baseline_smooth_radius(2.0F, 1.0e-3, 1, 1500) == 750);
    // Never zero, even for a baseline shorter than a sample.
    CHECK(frost::stats::baseline_smooth_radius(1.0e-6F, 1.0e-3, 1, 1000) == 1);
    CHECK_THROWS_AS(frost::stats::baseline_smooth_radius(0.0F, 1.0e-3, 1, 10),
                    std::invalid_argument);
    CHECK_THROWS_AS(frost::stats::baseline_smooth_radius(1.0F, 0.0, 1, 10),
                    std::invalid_argument);
}

TEST_CASE("BaselineFilterPlan removes a DC offset from a short series",
          "[stats][robust]") {
    // 1200 samples with a 1000-sample radius gave one knot, so the plan
    // returned untouched and the 5000 offset survived into the search.
    constexpr SizeType kCount = 1200;
    std::vector<float> row(kCount, 5000.0F);
    const auto radius =
        frost::stats::baseline_smooth_radius(2.0F, 1.0e-3, 1, kCount);
    frost::stats::BaselineFilterPlan plan;
    plan.execute(row.data(), kCount, radius);
    for (const float v : row) {
        REQUIRE(std::abs(v) < 1.0F);
    }
    CHECK_THROWS_AS(plan.execute(row.data(), kCount, 0), std::invalid_argument);
}

TEST_CASE("BaselineFilterPlan survives counts below one median-of-5 group",
          "[stats][robust]") {
    // Two knots on 4 or 5 samples makes count / 5 equal 0; the scratch
    // buffer must still be sized for what is written.
    for (SizeType count : {4U, 5U, 6U, 9U}) {
        std::vector<float> row(count, 100.0F);
        frost::stats::BaselineFilterPlan plan;
        const SizeType radius = std::max<SizeType>(count / 2, 1);
        CHECK_NOTHROW(plan.execute(row.data(), count, radius));
        for (const float v : row) {
            CHECK(std::abs(v) < 1.0F);
        }
    }
}

TEST_CASE("boxcar width list does not lose a width to float rounding",
          "[search][robust]") {
    // dmt uses ceil(wtsp * width), with at least one sample of progress.
    const auto widths = dmt::plans::generate_box_width_trials(64, 1.3F);
    REQUIRE_FALSE(widths.empty());
    CHECK(widths.front() == 1);
    for (std::size_t i = 1; i < widths.size(); ++i) {
        CHECK(widths[i] > widths[i - 1]);
    }
    CHECK(widths.back() <= 64);
    CHECK_THROWS_AS(dmt::plans::generate_box_width_trials(
                        64, std::numeric_limits<float>::quiet_NaN()),
                    std::invalid_argument);
}

TEST_CASE("giant_boxcar_width never shifts by the type width",
          "[search][robust]") {
    frost::search::Giant g;
    g.filter_idx = 3;
    CHECK(frost::search::giant_boxcar_width(g) == 8);
    g.filter_width = 24;
    CHECK(frost::search::giant_boxcar_width(g) == 24);
    g.filter_width = 0;
    g.filter_idx   = 200; // hand-built or corrupt: must saturate, not be UB
    CHECK(frost::search::giant_boxcar_width(g) ==
          (SizeType{1} << ((sizeof(SizeType) * 8) - 1)));
}

TEST_CASE("sub-band geometry helpers reject non-finite input",
          "[subband][robust]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(frost::search::subband_dm_step(700.0F, 1400.0F, 1.0e-3F) > 0.0);
    CHECK(frost::search::subband_dm_step(700.0F, 1400.0F, nan) == 0.0);
    CHECK(frost::search::subband_dm_step(nan, 1400.0F, 1.0e-3F) == 0.0);
    CHECK(frost::search::subband_dm_step(700.0F, nan, 1.0e-3F) == 0.0);
    CHECK(frost::search::subband_time_shift_samples(100.0F, 1400.0F, 700.0F,
                                                    nan) == 0.0);
}

TEST_CASE("cand_sep_dm_smear loads from TOML", "[config][robust]") {
    CHECK(load_toml("").search.cand_sep_dm_smear == 8.0F);
    CHECK(load_toml("[search]\ncand_sep_dm_smear = 0\n")
              .search.cand_sep_dm_smear == 0.0F);
    CHECK(load_toml("[search]\ncand_sep_dm_smear = 1.5\n")
              .search.cand_sep_dm_smear == 1.5F);
}

TEST_CASE("subband_dedup loads from TOML", "[config][robust]") {
    using frost::search::SubbandDedupMode;
    CHECK(load_toml("").search.subband_dedup == SubbandDedupMode::kTransitive);
    CHECK(load_toml("[search]\nsubband_dedup = \"seeded\"\n")
              .search.subband_dedup == SubbandDedupMode::kSeeded);
    CHECK(load_toml("[search]\nsubband_dedup = \"transitive\"\n")
              .search.subband_dedup == SubbandDedupMode::kTransitive);
    CHECK_THROWS(load_toml("[search]\nsubband_dedup = \"union\"\n"));
}
