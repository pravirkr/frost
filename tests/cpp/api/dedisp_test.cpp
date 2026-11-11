#include <cstddef>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <dmt/algorithms/fdmt_boxcar.hpp>

#include <frost/dedisp/dedisp.hpp>

using frost::dedisp::DedispPlanConfig;
using frost::dedisp::generate_dm_list;

namespace {

DedispPlanConfig make_config() {
    DedispPlanConfig cfg;
    cfg.nchans         = 16;
    cfg.dt             = 1.0e-3F; // 1 ms
    cfg.f0             = 1400.0F; // MHz
    cfg.df             = -1.0F;   // MHz, decreasing with channel index
    cfg.dm_min         = 0.0F;
    cfg.dm_max         = 50.0F;
    cfg.dm_pulse_width = 1000.0F; // microseconds
    cfg.dm_tol         = 1.25F;
    return cfg;
}

} // namespace

TEST_CASE("FDMT search depth includes staggered bands in the plan",
          "[dedisp][subband]") {
    auto cfg         = make_config();
    cfg.block_nsamps = 1024;
    for (const auto depth : {0U, 1U, 2U}) {
        cfg.subband_depth = depth;
        auto plan         = frost::dedisp::make_dmt_fdmt_plan(cfg);
        CHECK(plan.get_subband_depth() == depth);
        dmt::BoxcarConfig search;
        search.max_boxcar = 16;
        const dmt::algorithms::FDMTBoxcar engine(std::move(plan), search);
        const auto& planes         = engine.get_search_plans();
        const std::size_t expected = depth == 0 ? 1 : depth == 1 ? 4 : 11;
        CHECK(planes.size() == expected);
        std::size_t overlapping = 0;
        for (const auto& plane : planes) {
            if (plane.staggered) {
                ++overlapping;
                CHECK(plane.chan_begin > 0);
                CHECK(plane.chan_end < cfg.nchans);
            }
        }
        CHECK(overlapping == (depth == 0 ? 0 : depth == 1 ? 1 : 4));
    }
}

TEST_CASE("generate_dm_list spans [dm_min, dm_max] with increasing trials",
          "[dedisp]") {
    const auto cfg = make_config();
    const auto dms = generate_dm_list(cfg);

    REQUIRE(dms.size() >= 2);
    CHECK(dms.front() == cfg.dm_min);
    CHECK(dms.back() >= cfg.dm_max);
    for (std::size_t i = 1; i < dms.size(); ++i) {
        CHECK(dms[i] > dms[i - 1]);
    }
}

TEST_CASE("FDMT plan uses ascending channel edges for either axis",
          "[dedisp]") {
    auto cfg              = make_config();
    cfg.block_nsamps      = 1024;
    const auto descending = frost::dedisp::make_dmt_fdmt_plan(cfg);
    CHECK(descending.get_f_min() == 1384.5F);
    CHECK(descending.get_f_max() == 1400.5F);
    cfg.f0               = 1385.0F;
    cfg.df               = 1.0F;
    const auto ascending = frost::dedisp::make_dmt_fdmt_plan(cfg);
    CHECK(ascending.get_f_min() == descending.get_f_min());
    CHECK(ascending.get_f_max() == descending.get_f_max());
    CHECK(ascending.get_dm_grid_final() == descending.get_dm_grid_final());
    cfg.df = 0.0F;
    CHECK_THROWS_AS(frost::dedisp::make_dmt_fdmt_plan(cfg),
                    std::invalid_argument);
    cfg.df     = 1.0F;
    cfg.nchans = 1;
    CHECK_THROWS_AS(frost::dedisp::make_dmt_fdmt_plan(cfg),
                    std::invalid_argument);
    cfg.nchans = 16;
    cfg.f0     = 0.1F;
    CHECK_THROWS_AS(frost::dedisp::make_dmt_fdmt_plan(cfg),
                    std::invalid_argument);
}
