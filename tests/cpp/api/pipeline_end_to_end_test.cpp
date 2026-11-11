// End-to-end Pipeline tests on FDMT: DM recovery, valid-mode block
// carry, the final partial gulp, trailing-window sample indexing, manual
// channel zapping, and per-gulp output naming.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <frost/config/config.hpp>
#include <frost/dedisp/dedisp.hpp>
#include <frost/pipeline/input.hpp>
#include <frost/pipeline/pipeline.hpp>

#include "support/fake_filterbank.hpp"
#include "support/temp_dir.hpp"

using frost::SizeType;
using frost::testing::FakeFilterbankSpec;
using frost::testing::InjectedPulse;
using frost::testing::write_fake_filterbank;

namespace {

struct CandRow {
    float snr           = 0.0F;
    SizeType samp_idx   = 0;
    float time          = 0.0F;
    SizeType filter_idx = 0;
    SizeType dm_idx     = 0;
    float dm            = 0.0F;
};

std::vector<CandRow> read_cand_file(const std::filesystem::path& path) {
    std::vector<CandRow> rows;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream iss(line);
        CandRow r;
        iss >> r.snr >> r.samp_idx >> r.time >> r.filter_idx >> r.dm_idx >>
            r.dm;
        rows.push_back(r);
    }
    return rows;
}

std::filesystem::path fresh_dir(const std::string& name) {
    return frost::test::unique_temp_dir(name);
}

// Wide (256 MHz, 64-channel) L-band setup, where DM 300 sweeps ~310 samples.
FakeFilterbankSpec wide_band_spec(SizeType nsamps) {
    FakeFilterbankSpec spec;
    spec.nchans      = 64;
    spec.nsamps      = nsamps;
    spec.tsamp       = 0.001;
    spec.fch1        = 1400.0;
    spec.foff        = -4.0;
    spec.baseline    = 100;
    spec.noise_sigma = 5.0F;
    return spec;
}

frost::config::PipelineConfig
base_config(const std::filesystem::path& out_dir) {
    frost::config::PipelineConfig cfg;
    cfg.dedisp.dm_min         = 0.0F;
    cfg.dedisp.dm_max         = 500.0F;
    cfg.dedisp.dm_pulse_width = 1000.0F;
    cfg.search.boxcar_max     = 32;
    cfg.search.detect_thresh  = 7.0F;
    cfg.baseline_length       = 1.0F;
    cfg.nsamps_gulp           = 16384;
    cfg.output.output_dir     = out_dir.string();
    return cfg;
}

struct RunResult {
    std::vector<CandRow> rows;
    SizeType nsamps_processed = 0;
    SizeType max_delay        = 0;
    SizeType gulps            = 0;
};

RunResult run(const frost::config::PipelineConfig& cfg,
              const std::string& fil) {
    RunResult result;
    frost::config::InputConfig input;
    input.sigproc_file = fil;
    auto source        = frost::pipeline::open_input(input);
    auto engine        = frost::dedisp::make_dmt_fdmt_plan(
        frost::pipeline::make_dedisp_plan_config(cfg, source));
    result.max_delay = engine.get_dt_max();
    {
        frost::pipeline::Pipeline pipeline(cfg, source);
        while (pipeline.execute() != 0) {
            ++result.gulps;
        }
        result.nsamps_processed = pipeline.nsamps_processed();
    }
    if (!cfg.output.per_gulp_files) {
        result.rows = read_cand_file(
            std::filesystem::path(cfg.output.output_dir) / "candidates.cand");
    }
    return result;
}

// Candidates above threshold whose peak lies within `tol` samples of the
// pulse's FDMT arrival. FDMT times are referenced to the bottom of the
// band, so a pulse injected at `start` on the top channel is reported
// `dispersion_delay` samples later.
std::vector<CandRow> near(const FakeFilterbankSpec& spec,
                          const std::vector<CandRow>& rows,
                          const InjectedPulse& p,
                          float thresh,
                          SizeType tol) {
    const SizeType arrival = p.start + frost::testing::detail::dispersion_delay(
                                           spec, spec.nchans - 1, p.dm);
    std::vector<CandRow> hits;
    for (const auto& r : rows) {
        if (r.snr > thresh && r.samp_idx + tol >= arrival &&
            r.samp_idx <= arrival + p.width + tol) {
            hits.push_back(r);
        }
    }
    return hits;
}

const CandRow& best(const std::vector<CandRow>& rows) {
    return *std::max_element(
        rows.begin(), rows.end(),
        [](const auto& a, const auto& b) { return a.snr < b.snr; });
}

} // namespace

TEST_CASE("Pipeline recovers a dispersed pulse at its DM, time and width "
          "with RFI cleaning on",
          "[pipeline]") {
    const auto dir = fresh_dir("dm_recovery");
    const InjectedPulse pulse{6000, 4, 6.0F, 300.0F};
    const auto fil = write_fake_filterbank((dir / "in.fil").string(),
                                           wide_band_spec(16384), {pulse});
    auto cfg       = base_config(dir);

    const auto result = run(cfg, fil);
    const auto hits   = near(wide_band_spec(16384), result.rows, pulse,
                             cfg.search.detect_thresh, 4);
    REQUIRE(hits.size() == 1);
    const auto& b = hits.front();
    CHECK(std::abs(b.dm - pulse.dm) < 10.0F);
    CHECK(b.filter_idx == 2); // width 4
    CHECK(b.snr > 12.0F);
    CHECK(std::abs(b.time - static_cast<float>(b.samp_idx) * 0.001F) < 1.0e-5F);
}

TEST_CASE("Pipeline carries the gulp overlap: a pulse straddling a gulp "
          "boundary and one in the final partial gulp are each found once",
          "[pipeline]") {
    const auto dir  = fresh_dir("overlap");
    auto cfg        = base_config(dir);
    cfg.nsamps_gulp = 4096;

    const SizeType nsamps = (4 * 4096) + 1500; // ends in a partial gulp
    const auto spec       = wide_band_spec(nsamps);
    frost::config::InputConfig probe_input;
    probe_input.sigproc_file =
        write_fake_filterbank((dir / "probe.fil").string(), spec, {});
    auto probe = frost::pipeline::open_input(probe_input);
    const SizeType max_delay =
        frost::dedisp::make_dmt_fdmt_plan(
            frost::pipeline::make_dedisp_plan_config(cfg, probe))
            .get_dt_max();
    // The pulse crosses the actual FDMT input boundary.
    const SizeType first_boundary = cfg.nsamps_gulp;

    const std::vector<InjectedPulse> pulses = {
        {first_boundary - 2, 4, 6.0F, 200.0F}, // straddles gulp 1 -> 2
        {nsamps - 1500 + 200, 4, 6.0F, 50.0F}, // in the final partial gulp
    };
    const auto fil =
        write_fake_filterbank((dir / "in.fil").string(), spec, pulses);

    const auto result = run(cfg, fil);
    CHECK(result.gulps == 5);
    // The cold prefix is not searched; the end of the file is.
    CHECK(result.nsamps_processed == nsamps - max_delay);
    for (const auto& p : pulses) {
        const auto hits =
            near(spec, result.rows, p, cfg.search.detect_thresh, 4);
        CHECK(hits.size() == 1);
        if (!hits.empty()) {
            CHECK(std::abs(hits.front().dm - p.dm) < 10.0F);
        }
    }
}

TEST_CASE(
    "Pipeline reports trailing-window boxcar endpoints in absolute sample "
    "indices",
    "[pipeline]") {
    const auto dir        = fresh_dir("trailing_window");
    auto cfg              = base_config(dir);
    cfg.search.boxcar_max = 64;
    cfg.dedisp.dm_max     = 50.0F;

    const InjectedPulse pulse{9000, 32, 2.0F, 0.0F};
    const auto fil    = write_fake_filterbank((dir / "in.fil").string(),
                                              wide_band_spec(16384), {pulse});
    const auto result = run(cfg, fil);
    const auto hits   = near(wide_band_spec(16384), result.rows, pulse,
                             cfg.search.detect_thresh, 16);
    REQUIRE_FALSE(hits.empty());
    const auto& b = best(hits);
    CHECK(b.filter_idx >= 4); // detected on a wide (>= 16) filter
    CHECK(b.samp_idx >= pulse.start);
    CHECK(b.samp_idx <= pulse.start + pulse.width);
}

TEST_CASE("Pipeline zap_chans removes channels from dedispersion",
          "[pipeline]") {
    const auto dir = fresh_dir("zap");
    // Strong narrow-band, zero-DM burst confined to channels 10..19.
    InjectedPulse burst{5000, 8, 40.0F, 0.0F, 10, 19};
    const auto fil = write_fake_filterbank((dir / "in.fil").string(),
                                           wide_band_spec(16384), {burst});
    auto cfg       = base_config(dir);
    cfg.rfi.narrow.enabled = false;
    cfg.rfi.broad.enabled  = false;

    SECTION("without zapping the burst is detected") {
        const auto result = run(cfg, fil);
        CHECK_FALSE(near(wide_band_spec(16384), result.rows, burst,
                         cfg.search.detect_thresh, 8)
                        .empty());
    }
    SECTION("zapping its channels removes it") {
        cfg.rfi.zap_chans = {{8, 21}};
        const auto result = run(cfg, fil);
        CHECK(near(wide_band_spec(16384), result.rows, burst,
                   cfg.search.detect_thresh, 8)
                  .empty());
    }
}

TEST_CASE("Pipeline RFI cleaning removes a zero-DM broadband burst and "
          "keeps a dispersed pulse, with defaults or every strategy on",
          "[pipeline][rfi]") {
    const auto dir = fresh_dir("rfi_burst");
    const InjectedPulse pulse{6000, 4, 6.0F, 300.0F};
    const InjectedPulse burst{11000, 4, 40.0F, 0.0F};
    const auto fil = write_fake_filterbank(
        (dir / "in.fil").string(), wide_band_spec(16384), {pulse, burst});
    auto cfg = base_config(dir);

    SECTION("without cleaning the burst is a candidate") {
        cfg.rfi.narrow.enabled = false;
        cfg.rfi.broad.enabled  = false;
        const auto result      = run(cfg, fil);
        CHECK_FALSE(near(wide_band_spec(16384), result.rows, burst,
                         cfg.search.detect_thresh, 8)
                        .empty());
    }
    SECTION("default RFI") {
        const auto result = run(cfg, fil);
        CHECK(near(wide_band_spec(16384), result.rows, burst,
                   cfg.search.detect_thresh, 8)
                  .empty());
        const auto hits = near(wide_band_spec(16384), result.rows, pulse,
                               cfg.search.detect_thresh, 4);
        REQUIRE(hits.size() == 1);
        CHECK(std::abs(hits.front().dm - pulse.dm) < 10.0F);
    }
    SECTION("every strategy enabled") {
        cfg.rfi.dropout.enabled         = true;
        cfg.rfi.moments.enabled         = true;
        cfg.rfi.iqrm.enabled            = true;
        cfg.rfi.mask_grow               = 1;
        cfg.rfi.kadane_channel.enabled  = true;
        cfg.rfi.kadane_spectrum.enabled = true;
        cfg.rfi.zero_dm   = frost::prepare::rfi::ZeroDMMode::kZdot;
        cfg.rfi.equalize  = true;
        const auto result = run(cfg, fil);
        CHECK(near(wide_band_spec(16384), result.rows, burst,
                   cfg.search.detect_thresh, 8)
                  .empty());
        const auto hits = near(wide_band_spec(16384), result.rows, pulse,
                               cfg.search.detect_thresh, 4);
        REQUIRE(hits.size() == 1);
        CHECK(std::abs(hits.front().dm - pulse.dm) < 10.0F);
        CHECK(hits.front().snr > 10.0F);
    }
}

TEST_CASE("Pipeline per_gulp_files writes per-gulp .cand files", "[pipeline]") {
    const auto dir            = fresh_dir("per_gulp");
    auto cfg                  = base_config(dir);
    cfg.nsamps_gulp           = 8192;
    cfg.output.per_gulp_files = true;
    cfg.output.beam           = 3;

    const auto fil = write_fake_filterbank(
        (dir / "in.fil").string(), wide_band_spec(16384),
        {{2000, 4, 6.0F, 100.0F}, {12000, 4, 6.0F, 80.0F}});
    run(cfg, fil);

    // tstart = MJD 58000.0 = 2017-09-04 00:00:00 UTC. The second gulp's
    // peak is in the second input gulp.
    const auto first = dir / "2017-09-04-00:00:00_03.cand";
    REQUIRE(std::filesystem::exists(first));
    CHECK_FALSE(read_cand_file(first).empty());
    SizeType cand_files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        cand_files += entry.path().extension() == ".cand" ? 1 : 0;
    }
    CHECK(cand_files == 2);
}
