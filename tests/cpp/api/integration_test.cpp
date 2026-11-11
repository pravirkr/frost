#include <cstdio>
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
using frost::testing::InjectedPulse;
using frost::testing::write_fake_filterbank;

namespace {

struct CandRow {
    float snr;
    SizeType samp_idx;
    SizeType filter_idx;
};

std::vector<CandRow> read_cand_file(const std::string& path) {
    std::vector<CandRow> rows;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream iss(line);
        CandRow row{};
        float time = 0.0F;
        iss >> row.snr >> row.samp_idx >> time >> row.filter_idx;
        rows.push_back(row);
    }
    return rows;
}

} // namespace

TEST_CASE("End-to-end: multiple injected pulses at different times/widths/"
          "SNRs are all recovered in one gulp",
          "[integration]") {
    constexpr SizeType kNChans = 32;
    constexpr SizeType kNSamps = 16384;

    // Three well-separated pulses (far apart relative to cand_sep_time
    // and boxcar_max so they land in distinct candidates), with
    // increasing width and amplitude, on top of real Gaussian noise
    // (unlike the single-pulse pipeline_test fixture, which is
    // deliberately noiseless).
    const std::vector<InjectedPulse> pulses = {
        {2000, 2, 40.0F},
        {7000, 8, 60.0F},
        {12000, 16, 80.0F},
    };

    const auto work_dir = frost::test::unique_temp_dir("integration");
    const std::string fil_path =
        write_fake_filterbank((work_dir / "integration_test.fil").string(),
                              kNChans, kNSamps, /*baseline=*/100,
                              /*noise_sigma=*/5.0F, pulses);

    frost::config::PipelineConfig cfg;
    cfg.input.sigproc_file = fil_path;
    cfg.rfi.broad.enabled  = false;
    cfg.rfi.narrow.enabled = false;

    cfg.dedisp.dm_min         = 0.0F;
    cfg.dedisp.dm_max         = 20.0F; // small DM range -> small max_delay
    cfg.dedisp.dm_tol         = 1.25F;
    cfg.dedisp.dm_pulse_width = 1000.0F;

    cfg.search.boxcar_max      = 32;
    cfg.search.detect_thresh   = 6.0F;
    cfg.search.cand_sep_time   = 5;
    cfg.search.cand_sep_filter = 3;
    cfg.search.cand_sep_dm = 1'000'000;  // merge across all (identical) DM rows
    cfg.search.cand_sep_dm_smear = 0.0F; // trial-count merge only
    cfg.search.max_giant_rate    = 0.0F;

    cfg.nsamps_gulp       = kNSamps;
    cfg.baseline_length   = 1.0F;
    cfg.output.output_dir = work_dir.string();

    const std::string cand_path = (work_dir / "candidates.cand").string();

    {
        auto source = frost::pipeline::open_input(cfg.input);

        auto dedisperser = frost::dedisp::make_dmt_fdmt_plan(
            frost::pipeline::make_dedisp_plan_config(cfg, source));
        REQUIRE(kNSamps > dedisperser.get_dt_max() + cfg.search.boxcar_max);

        frost::pipeline::Pipeline pipeline(cfg, source);
        while (pipeline.execute() != 0) {
        }
    }

    const auto rows = read_cand_file(cand_path);
    REQUIRE(rows.size() >= pulses.size());

    // For every injected pulse, some candidate should land near its
    // time span with a plausible filter_idx (log2(width), +/- 1 to
    // allow for the boxcar sweep's power-of-2 granularity) and pass
    // the detection threshold.
    for (const auto& pulse : pulses) {
        bool found = false;
        for (const auto& row : rows) {
            const bool time_ok =
                row.samp_idx + cfg.search.boxcar_max >= pulse.start &&
                row.samp_idx <=
                    pulse.start + pulse.width + cfg.search.boxcar_max;
            if (!time_ok) {
                continue;
            }
            if (row.snr > cfg.search.detect_thresh) {
                found = true;
                break;
            }
        }
        CHECK(found);
    }

    std::remove(fil_path.c_str());
    std::remove(cand_path.c_str());
}
