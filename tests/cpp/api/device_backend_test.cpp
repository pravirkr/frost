#include <cstdio>
#include <stdexcept>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <dmt/algorithms/fdmt_boxcar.hpp>
#include <dmt/common/backend.hpp>
#include <sys/wait.h>

#include <frost/dedisp/dedisp.hpp>
#include <frost/prepare/rfi.hpp>

using Catch::Matchers::ContainsSubstring;
using frost::dedisp::DedispPlanConfig;
using frost::dedisp::make_dmt_fdmt_plan;

namespace {

DedispPlanConfig tiny_plan() {
    DedispPlanConfig cfg;
    cfg.nchans         = 8;
    cfg.dt             = 1.0e-3F;
    cfg.f0             = 1500.0F;
    cfg.df             = -1.0F;
    cfg.dm_min         = 0.0F;
    cfg.dm_max         = 10.0F;
    cfg.dm_pulse_width = 1000.0F;
    cfg.dm_tol         = 1.25F;
    cfg.dm_dtype       = frost::DataType::kFloat32;
    cfg.block_nsamps   = 64;
    return cfg;
}

struct Cmd {
    int status = 0;
    std::string output;
};

Cmd run_frost(const std::string& args) {
    const std::string command =
        std::string(FROST_APP_PATH) + " " + args + " 2>&1";
    Cmd result;
    FILE* pipe = popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe) != nullptr) {
        result.output += buf;
    }
    const int closed = pclose(pipe);
    result.status    = WEXITSTATUS(closed);
    return result;
}

} // namespace

TEST_CASE("dmt::parse_backend accepts cpu, cuda and hip", "[device]") {
    CHECK(dmt::parse_backend("cpu") == dmt::Backend::kCPU);
    CHECK(dmt::parse_backend("cuda") == dmt::Backend::kCUDA);
    CHECK(dmt::parse_backend("hip") == dmt::Backend::kHIP);
    CHECK_THROWS_AS(dmt::parse_backend("gpu"), std::invalid_argument);
    CHECK_THROWS_WITH(dmt::parse_backend("nope"),
                      ContainsSubstring("Unknown backend"));
}

TEST_CASE("FDMT engine construction rejects a backend this build lacks",
          "[device]") {
    const auto cfg         = tiny_plan();
    const auto make_engine = [&](dmt::Backend backend, int device) {
        return dmt::algorithms::FDMTBoxcar(
            make_dmt_fdmt_plan(cfg), dmt::BoxcarConfig(8), true,
            {.backend = backend, .device = device});
    };
    auto cpu = make_engine(dmt::Backend::kCPU, 0);
    CHECK(cpu.backend() == dmt::Backend::kCPU);
    for (const auto backend : {dmt::Backend::kCUDA, dmt::Backend::kHIP}) {
        if (!dmt::is_available(backend)) {
            CHECK_THROWS_AS(make_engine(backend, 0), std::invalid_argument);
            CHECK_THROWS_WITH(make_engine(backend, 0),
                              ContainsSubstring("not available"));
        }
    }
}

TEST_CASE("frost --dedisp-backend aliases --device and --device-id",
          "[device]") {
    const auto unknown = run_frost("--device nope --file missing.fil");
    CHECK(unknown.status != 0);
    CHECK_THAT(unknown.output, ContainsSubstring("Unknown backend"));

    const auto alias = run_frost("--dedisp-backend hip --device-id -3");
    CHECK(alias.status != 0);
    CHECK_THAT(alias.output, ContainsSubstring("device-id"));

    const auto id = run_frost("--device cuda --device-id 1 --dm-nbits 8");
    CHECK(id.status != 0);
    CHECK_THAT(id.output, ContainsSubstring("dm-nbits"));

    const auto help = run_frost("--help");
    CHECK(help.status == 0);
    CHECK_THAT(help.output, ContainsSubstring("--device"));
    CHECK_THAT(help.output, ContainsSubstring("--dedisp-backend"));
    CHECK_THAT(help.output, ContainsSubstring("--device-id"));
}
