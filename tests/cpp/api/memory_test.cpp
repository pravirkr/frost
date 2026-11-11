// Memory pre-flight: the estimate tracks stage workspaces and the check
// refuses, or lowers a default gulp, before anything is allocated.
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <dmt/algorithms/fdmt_boxcar.hpp>
#include <omp.h>

#include <frost/dedisp/dedisp.hpp>
#include <frost/pipeline/input.hpp>
#include <frost/pipeline/memory.hpp>
#include <frost/pipeline/pipeline.hpp>

#include "support/fake_filterbank.hpp"
#include "support/temp_dir.hpp"

using Catch::Matchers::ContainsSubstring;
using frost::SizeType;
using frost::config::PipelineConfig;
using frost::pipeline::check_host_memory;
using frost::pipeline::estimate_host_memory;
using frost::pipeline::max_gulp_within;

namespace {

constexpr SizeType kMiB = SizeType{1} << 20U;
constexpr SizeType kGiB = SizeType{1} << 30U;

} // namespace

TEST_CASE("estimate_host_memory counts normalized input and engine state",
          "[memory]") {
    PipelineConfig cfg;
    cfg.search.boxcar_max = 128;
    cfg.nsamps_gulp       = 8192;
    const auto base       = estimate_host_memory(cfg, 6656, 338, 6656, 16);
    CHECK(base.per_sample_bytes >= 10 * 6656);
    // Streaming FDMTBoxcar allocates no full DM-time output plane: increasing
    // ndm affects only fixed boxcar history, peaks and lookup tables.
    const auto more_dm = estimate_host_memory(cfg, 6656, 676, 6656, 16);
    CHECK(more_dm.per_sample_bytes == base.per_sample_bytes);
    CHECK(more_dm.fixed_bytes > base.fixed_bytes);
    const auto state =
        estimate_host_memory(cfg, 6656, 338, 6656, 16, 128 * kMiB, 32 * kMiB);
    CHECK(state.fixed_bytes >= base.fixed_bytes + 160 * kMiB);
    const auto single = estimate_host_memory(cfg, 6656, 338, 6656, 1);
    CHECK(single.per_sample_bytes == base.per_sample_bytes);
    CHECK(single.fixed_bytes < base.fixed_bytes);
    cfg.search.max_candidates *= 2;
    CHECK(estimate_host_memory(cfg, 6656, 338, 6656, 16).fixed_bytes >
          base.fixed_bytes);
}

TEST_CASE("max_gulp_within inverts the estimate", "[memory]") {
    PipelineConfig cfg;
    cfg.search.boxcar_max = 128;
    cfg.nsamps_gulp       = 4096;
    const auto est        = estimate_host_memory(cfg, 3328, 524, 3328, 16);
    const auto gulp       = max_gulp_within(est, 8 * kGiB);
    cfg.nsamps_gulp       = gulp;
    const auto at_gulp    = estimate_host_memory(cfg, 3328, 524, 3328, 16);
    CHECK(at_gulp.total_bytes <= 8 * kGiB + kGiB / 8); // slot term moves a bit
    CHECK(gulp > 30000);
    CHECK(max_gulp_within(est, est.fixed_bytes) == 0);
}

TEST_CASE("memory fallback budgets staggered sub-band planes", "[memory]") {
    PipelineConfig cfg;
    cfg.search.boxcar_max = 16;
    for (const auto depth : {0U, 1U, 2U}) {
        cfg.search.subband_levels = depth;
        const SizeType planes     = depth == 0 ? 1 : depth == 1 ? 4 : 11;
        const auto fallback       = estimate_host_memory(cfg, 16, 10, 16, 1);
        const auto exact_bound =
            estimate_host_memory(cfg, 16, 10, 16, 1, 0, 0, 10 * planes);
        CHECK(fallback.total_bytes == exact_bound.total_bytes);
    }
}

TEST_CASE("check_host_memory lowers a default gulp and refuses an explicit one",
          "[memory][robust]") {
    const auto dir = frost::test::unique_temp_dir("memory");
    frost::testing::FakeFilterbankSpec spec;
    spec.nchans      = 512;
    spec.nsamps      = 4096;
    spec.noise_sigma = 5.0F;
    const auto fil   = frost::testing::write_fake_filterbank(
        (dir / "m.fil").string(), spec, {});
    frost::config::InputConfig in;
    in.sigproc_file = fil;
    auto source     = frost::pipeline::open_input(in);

    PipelineConfig cfg; // default gulp 262144
    cfg.dedisp.dm_max     = 50.0F;
    cfg.search.boxcar_max = 64;

    // Ample memory: unchanged.
    auto roomy = cfg;
    auto ok    = check_host_memory(roomy, source, true, 1024 * kGiB);
    CHECK_FALSE(ok.lowered);
    CHECK(roomy.nsamps_gulp == cfg.nsamps_gulp);

    // 2 GiB available: the default gulp is lowered to fit.
    auto small = cfg;
    auto lower = check_host_memory(small, source, true, 2 * kGiB);
    CHECK(lower.lowered);
    CHECK(small.nsamps_gulp < cfg.nsamps_gulp);
    CHECK(small.nsamps_gulp % 1024 == 0);
    CHECK(lower.estimate.total_bytes <= (2 * kGiB) - ((2 * kGiB) / 10));

    // The same shortfall with an explicit gulp is an error that names the fix.
    auto explicit_cfg = cfg;
    CHECK_THROWS_WITH(check_host_memory(explicit_cfg, source, false, 2 * kGiB),
                      ContainsSubstring("largest nsamps_gulp that fits"));
    CHECK(explicit_cfg.nsamps_gulp == cfg.nsamps_gulp);

    // Nothing fits at all.
    auto tiny = cfg;
    CHECK_THROWS_WITH(check_host_memory(tiny, source, true, 64 * kMiB),
                      ContainsSubstring("not even nsamps_gulp"));
}

namespace {

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << text;
}

std::string bytes(SizeType value) { return std::to_string(value) + "\n"; }

} // namespace

TEST_CASE("available_host_memory follows this process's cgroup",
          "[memory][robust]") {
    const auto root = frost::test::unique_temp_dir("memory_cgroup");
    const auto proc = root / "proc";
    const auto cg   = root / "cgroup";
    write_file(proc / "meminfo", "MemTotal: 209715200 kB\n"
                                 "MemAvailable: 104857600 kB\n");
    using frost::pipeline::available_host_memory;

    SECTION("no cgroup limit: MemAvailable") {
        write_file(proc / "self" / "cgroup", "0::/\n");
        write_file(cg / "memory.max", "max\n");
        CHECK(available_host_memory(proc, cg) == 100 * kGiB);
    }
    SECTION("v2: the tightest ancestor limit, page cache not counted") {
        write_file(proc / "self" / "cgroup", "0::/user.slice/job\n");
        write_file(cg / "memory.max", "max\n");
        write_file(cg / "user.slice" / "memory.max", bytes(8 * kGiB));
        write_file(cg / "user.slice" / "memory.current", bytes(3 * kGiB));
        write_file(cg / "user.slice" / "memory.stat",
                   "anon 1073741824\ninactive_file " + bytes(2 * kGiB));
        write_file(cg / "user.slice" / "job" / "memory.max", "max\n");
        CHECK(available_host_memory(proc, cg) == 7 * kGiB);
    }
    SECTION("v1: the memory controller's own path") {
        write_file(proc / "self" / "cgroup",
                   "5:cpu,cpuacct:/other\n4:memory:/slurm/job1\n");
        write_file(cg / "memory" / "memory.limit_in_bytes",
                   "9223372036854771712\n");
        const auto job = cg / "memory" / "slurm" / "job1";
        write_file(job / "memory.limit_in_bytes", bytes(4 * kGiB));
        write_file(job / "memory.usage_in_bytes", bytes(kGiB));
        write_file(job / "memory.stat",
                   "cache 1\ntotal_inactive_file " + bytes(kGiB / 2));
        CHECK(available_host_memory(proc, cg) == (3 * kGiB) + (kGiB / 2));
    }
    std::filesystem::remove_all(root);
}

TEST_CASE("check_host_memory never lowers the gulp below max_delay + "
          "boxcar_max",
          "[memory][robust]") {
    const auto dir = frost::test::unique_temp_dir("memory_delay");
    frost::testing::FakeFilterbankSpec spec;
    spec.nchans      = 512;
    spec.nsamps      = 1024;
    spec.fch1        = 1400.0;
    spec.foff        = -1.0;
    spec.noise_sigma = 5.0F;
    const auto fil   = frost::testing::write_fake_filterbank(
        (dir / "d.fil").string(), spec, {});
    frost::config::InputConfig in;
    in.sigproc_file = fil;
    auto source     = frost::pipeline::open_input(in);

    PipelineConfig cfg; // default gulp 262144
    cfg.dedisp.dm_max     = 2000.0F;
    cfg.search.boxcar_max = 64;
    const auto engine     = frost::dedisp::make_dmt_fdmt_plan(
        frost::pipeline::make_dedisp_plan_config(cfg, source));
    const SizeType needed = engine.get_dt_max() + cfg.search.boxcar_max;
    REQUIRE(needed > 4096); // multiple 1024-sample rounding steps

    bool lowered = false;
    bool refused = false;
    for (SizeType mib = 256; mib <= 4096; mib += 32) {
        auto trial = cfg;
        try {
            const auto check =
                check_host_memory(trial, source, true, mib * kMiB);
            if (check.lowered) {
                lowered = true;
                CHECK(trial.nsamps_gulp > needed);
            }
        } catch (const std::runtime_error& e) {
            refused = true;
            CHECK_THAT(e.what(), ContainsSubstring("max_delay"));
        }
    }
    CHECK(lowered);
    CHECK(refused);
    std::filesystem::remove_all(dir);
}

TEST_CASE("backend memory budget controls only the selected memory pool",
          "[memory]") {
    const auto dir = frost::test::unique_temp_dir("backend_budget");
    frost::testing::FakeFilterbankSpec spec;
    spec.nchans = 16;
    spec.nsamps = 1024;
    PipelineConfig cfg;
    cfg.input.sigproc_file = frost::testing::write_fake_filterbank(
        (dir / "in.fil").string(), spec, {});
    cfg.nsamps_gulp       = 4096;
    cfg.search.boxcar_max = 16;
    cfg.dedisp.dm_max     = 40.0F;
    auto source           = frost::pipeline::open_input(cfg.input);
    cfg.memory_limit_mib  = 1;
    CHECK_THROWS_AS(frost::pipeline::check_memory(cfg, source, false, 4 * kGiB),
                    std::runtime_error);
    cfg.memory_limit_mib = 256;
    CHECK_THROWS_AS(frost::pipeline::check_memory(cfg, source, false, 4 * kGiB),
                    std::runtime_error);
    // This is a pure planning check: injecting device availability exercises
    // the GPU budget without constructing an engine or requiring a toolkit.
    cfg.dedisp.device = dmt::Backend::kCUDA;
    const auto gpu =
        frost::pipeline::check_memory(cfg, source, false, 4 * kGiB);
    CHECK(gpu.device_memory);
    CHECK(gpu.estimate.postprocess_bytes == 0);
    CHECK(gpu.available_bytes == 256 * kMiB);
    CHECK(gpu.estimate.total_bytes < gpu.available_bytes);
    std::filesystem::remove_all(dir);
}
TEST_CASE("synchronous input budgets one raw buffer", "[memory]") {
    PipelineConfig cfg;
    const auto async = estimate_host_memory(cfg, 16, 10, 16, 1);
    cfg.async_io     = false;
    const auto sync  = estimate_host_memory(cfg, 16, 10, 16, 1);
    CHECK(async.input_bytes == 2 * sync.input_bytes);
    CHECK(async.total_bytes > sync.total_bytes);
    CHECK(async.total_bytes == async.input_bytes + async.rfi_bytes +
                                   async.fdmt_bytes + async.postprocess_bytes +
                                   async.reserve_bytes);
}

TEST_CASE("FDMT preflight bounds reported allocations at every sub-band depth",
          "[memory][dedisp]") {
    const auto dir = frost::test::unique_temp_dir("fdmt_budget");
    struct RestoreThreads {
        int count = omp_get_max_threads();
        ~RestoreThreads() { omp_set_num_threads(count); }
    } restore;
    for (const auto nchans : {16U, 24U, 64U}) {
        frost::testing::FakeFilterbankSpec spec;
        spec.nchans = nchans;
        spec.nsamps = 4096;
        PipelineConfig cfg;
        cfg.input.sigproc_file = frost::testing::write_fake_filterbank(
            (dir / "in.fil").string(), spec, {});
        cfg.search.boxcar_max     = 16;
        cfg.search.max_candidates = 64;
        cfg.dedisp.dm_max         = 200.0F;
        auto source               = frost::pipeline::open_input(cfg.input);
        for (const auto threads : {1, 4}) {
            omp_set_num_threads(threads);
            for (const auto gulp : {128U, 4096U}) {
                cfg.nsamps_gulp = gulp;
                for (const auto depth : {0U, 1U, 2U}) {
                    cfg.search.subband_levels = depth;
                    INFO("channels=" << nchans << " threads=" << threads
                                     << " gulp=" << gulp << " depth=" << depth);
                    const auto budget = frost::pipeline::check_memory(
                        cfg, source, false, 4 * kGiB);
                    dmt::BoxcarConfig search;
                    search.max_boxcar     = cfg.search.boxcar_max;
                    search.max_candidates = cfg.search.max_candidates;
                    const dmt::algorithms::FDMTBoxcar engine(
                        frost::dedisp::make_dmt_fdmt_plan(
                            frost::pipeline::make_dedisp_plan_config(cfg,
                                                                     source)),
                        search, true, {.nthreads = threads});
                    CHECK(engine.get_memory_usage().total() <=
                          budget.estimate.fdmt_bytes);
                }
            }
        }
    }
    std::filesystem::remove_all(dir);
}
