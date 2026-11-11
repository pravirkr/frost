#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <dmt/common/backend.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <frost/config/config.hpp>
#include <frost/pipeline/input.hpp>
#include <frost/pipeline/pipeline.hpp>

#include "support/fake_filterbank.hpp"
#include "support/temp_dir.hpp"

using frost::SizeType;

namespace {

void write_string_field(std::ostream& os, const std::string& s) {
    const auto len = static_cast<int32_t>(s.size());
    os.write(reinterpret_cast<const char*>(&len), sizeof(len));
    os.write(s.data(), len);
}

template <typename T>
void write_kv(std::ostream& os, const std::string& key, const T& value) {
    write_string_field(os, key);
    os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

std::string write_filterbank(const std::filesystem::path& path,
                             SizeType nchans,
                             SizeType nsamps,
                             SizeType pulse_start,
                             SizeType pulse_width,
                             uint8_t baseline,
                             uint8_t amplitude) {
    std::ofstream os(path, std::ios::binary);
    write_string_field(os, "HEADER_START");
    write_kv(os, "telescope_id", 4);
    write_kv(os, "machine_id", 10);
    write_kv(os, "data_type", 1);
    write_kv(os, "fch1", 1400.0);
    write_kv(os, "foff", -1.0);
    write_kv(os, "nchans", static_cast<int>(nchans));
    write_kv(os, "nbits", 8);
    write_kv(os, "nifs", 1);
    write_kv(os, "tsamp", 0.001);
    write_kv(os, "tstart", 58000.0);
    write_kv(os, "ibeam", 0);
    write_string_field(os, "HEADER_END");
    std::vector<uint8_t> row(nchans, baseline);
    for (SizeType t = 0; t < nsamps; ++t) {
        const uint8_t value =
            (t >= pulse_start && t < pulse_start + pulse_width)
                ? static_cast<uint8_t>(baseline + amplitude)
                : baseline;
        std::fill(row.begin(), row.end(), value);
        os.write(reinterpret_cast<const char*>(row.data()),
                 static_cast<std::streamsize>(row.size()));
    }
    return path.string();
}

dmt::Backend first_gpu_backend() {
    for (const auto backend : dmt::available_backends()) {
        if (backend != dmt::Backend::kCPU)
            return backend;
    }
    return dmt::Backend::kCPU;
}
} // namespace

TEST_CASE("device search reports the injected pulse sample", "[gpu][parity]") {
    const auto backend = first_gpu_backend();
    if (backend == dmt::Backend::kCPU) {
        SKIP("no CUDA or HIP backend in this build");
    }
    constexpr SizeType kChans      = 32;
    constexpr SizeType kSamps      = 4096;
    constexpr SizeType kPulseStart = 2000;
    constexpr SizeType kPulseWidth = 4;
    const auto root                = frost::test::unique_temp_dir("gpu_parity");
    const auto fil = write_filterbank(root / "pulse.fil", kChans, kSamps,
                                      kPulseStart, kPulseWidth, 100, 80);

    auto run = [&](dmt::Backend which, const std::filesystem::path& out_dir) {
        frost::config::PipelineConfig cfg;
        cfg.input.sigproc_file       = fil;
        cfg.dedisp.device            = which;
        cfg.dedisp.device_id         = 0;
        cfg.dedisp.dm_min            = 0.0F;
        cfg.dedisp.dm_max            = 20.0F;
        cfg.dedisp.dm_tol            = 1.25F;
        cfg.dedisp.dm_pulse_width    = 1000.0F;
        cfg.rfi.broad.enabled        = false;
        cfg.rfi.narrow.enabled       = false;
        cfg.search.boxcar_max        = 16;
        cfg.search.detect_thresh     = 5.0F;
        cfg.search.cand_sep_dm       = 1000000;
        cfg.search.cand_sep_dm_smear = 0.0F; // trial-count merge only
        cfg.search.cluster           = true;
        cfg.nsamps_gulp              = kSamps;
        cfg.baseline_length          = 0.5F;
        cfg.output.output_dir        = out_dir.string();
        cfg.output.per_gulp_files    = false;
        std::filesystem::create_directories(out_dir);
        std::filesystem::remove(out_dir / "candidates.cand");
        frost::config::InputConfig input;
        input.sigproc_file = fil;
        auto source        = frost::pipeline::open_input(input);
        frost::pipeline::Pipeline pipeline(cfg, source);
        while (pipeline.execute() != 0) {
        }
        std::ifstream cand(out_dir / "candidates.cand");
        REQUIRE(cand.is_open());
        float best_snr = -1.0F;
        SizeType best  = 0;
        std::string line;
        while (std::getline(cand, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            std::istringstream row(line);
            float snr     = 0.0F;
            SizeType samp = 0;
            row >> snr >> samp;
            if (snr > best_snr) {
                best_snr = snr;
                best     = samp;
            }
        }
        REQUIRE(best_snr > 0.0F);
        return best;
    };

    const SizeType cpu_samp = run(dmt::Backend::kCPU, root / "cpu");
    const SizeType gpu_samp = run(backend, root / "gpu");
    CHECK(cpu_samp == kPulseStart + kPulseWidth - 1);
    CHECK(gpu_samp == cpu_samp);
}

namespace {

using CandRows = std::vector<std::vector<double>>;

CandRows run_cand_file(const std::string& fil,
                       dmt::Backend which,
                       const std::filesystem::path& out_dir,
                       SizeType levels,
                       bool cluster,
                       bool rfi) {
    frost::config::PipelineConfig cfg;
    cfg.rfi.broad.enabled     = rfi;
    cfg.rfi.narrow.enabled    = rfi;
    cfg.input.sigproc_file    = fil;
    cfg.dedisp.device         = which;
    cfg.dedisp.device_id      = 0;
    cfg.dedisp.dm_min         = 0.0F;
    cfg.dedisp.dm_max         = 80.0F;
    cfg.dedisp.dm_tol         = 1.25F;
    cfg.dedisp.dm_pulse_width = 1000.0F;
    cfg.search.boxcar_max     = 32;
    cfg.search.detect_thresh  = 5.0F;
    cfg.search.cand_sep_dm    = 5;
    cfg.search.cluster        = cluster;
    cfg.search.subband_levels = levels;
    cfg.nsamps_gulp           = 8192;
    cfg.baseline_length       = 0.5F;
    cfg.output.output_dir     = out_dir.string();
    cfg.output.per_gulp_files = false;
    std::filesystem::create_directories(out_dir);
    std::filesystem::remove(out_dir / "candidates.cand");
    frost::config::InputConfig input;
    input.sigproc_file = fil;
    auto source        = frost::pipeline::open_input(input);
    {
        frost::pipeline::Pipeline pipeline(cfg, source);
        while (pipeline.execute() != 0) {
        }
    }
    CandRows rows;
    std::ifstream cand(out_dir / "candidates.cand");
    REQUIRE(cand.is_open());
    std::string line;
    while (std::getline(cand, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream in(line);
        std::vector<double> row;
        double v = 0.0;
        while (in >> v) {
            row.push_back(v);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace

// Every sub-band level must run on the device path (finalization must follow
// plane collection). Candidate files must agree row for row.
TEST_CASE("device pipeline matches the CPU at every sub-band level",
          "[gpu][parity]") {
    const auto backend = first_gpu_backend();
    if (backend == dmt::Backend::kCPU) {
        SKIP("no CUDA or HIP backend in this build");
    }
    frost::testing::FakeFilterbankSpec spec;
    spec.nchans                                             = 64;
    spec.nsamps                                             = 16384;
    spec.baseline                                           = 100;
    spec.noise_sigma                                        = 3.0F;
    const std::vector<frost::testing::InjectedPulse> pulses = {
        {3000, 6, 40.0F, 40.0F},
        {9000, 6, 60.0F, 30.0F, 0, 15},
        {12000, 10, 30.0F, 55.0F},
    };
    const auto root = frost::test::unique_temp_dir("gpu_parity_levels");
    const auto fil  = frost::testing::write_fake_filterbank(
        (root / "levels.fil").string(), spec, pulses);

    for (const SizeType levels : {SizeType{0}, SizeType{1}, SizeType{2}}) {
        for (const bool rfi : {false, true}) {
            for (const bool cluster : {true, false}) {
                CAPTURE(levels, cluster, rfi);
                const auto tag = std::to_string(levels) +
                                 (cluster ? "c" : "n") + (rfi ? "r" : "x");
                const auto cpu =
                    run_cand_file(fil, dmt::Backend::kCPU, root / ("cpu" + tag),
                                  levels, cluster, rfi);
                const auto gpu = run_cand_file(
                    fil, backend, root / ("gpu" + tag), levels, cluster, rfi);
                if (!rfi) { // default RFI excision may legitimately zap these
                            // pulses
                    REQUIRE_FALSE(cpu.empty());
                }
                REQUIRE(cpu.size() == gpu.size());
                for (SizeType r = 0; r < cpu.size(); ++r) {
                    REQUIRE(cpu[r].size() == gpu[r].size());
                    CHECK(std::abs(cpu[r][0] - gpu[r][0]) <=
                          1.0e-3 * std::abs(cpu[r][0]));
                    for (SizeType c = 1; c < cpu[r].size(); ++c) {
                        if (c == 13) { // snr_best: float, like column 0
                            CHECK(std::abs(cpu[r][c] - gpu[r][c]) <=
                                  1.0e-3 * std::abs(cpu[r][c]));
                        } else if (c == 2 || c == 5 || c == 9 || c == 10 ||
                                   c == 18 || c == 19 || c == 21 ||
                                   c == 22) { // time, DM and frequency floats
                            CHECK(cpu[r][c] ==
                                  Catch::Approx(gpu[r][c]).epsilon(1e-4));
                        } else {
                            CHECK(cpu[r][c] == gpu[r][c]);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("device search survives more giants than the per-DM cap",
          "[gpu][robust]") {
    const auto backend = first_gpu_backend();
    if (backend == dmt::Backend::kCPU) {
        SKIP("no CUDA or HIP backend in this build");
    }
    // A threshold far below the noise floor makes nearly every sample a
    // detection, well beyond 4096 per DM trial. The run must complete, warn,
    // and still write candidates.
    frost::testing::FakeFilterbankSpec spec;
    spec.nchans      = 32;
    spec.nsamps      = 16384;
    spec.noise_sigma = 10.0F;
    const auto root  = frost::test::unique_temp_dir("gpu_giant_cap");
    const auto fil   = frost::testing::write_fake_filterbank(
        (root / "noise.fil").string(), spec, {});

    frost::config::PipelineConfig cfg;
    cfg.input.sigproc_file    = fil;
    cfg.dedisp.device         = backend;
    cfg.dedisp.dm_min         = 0.0F;
    cfg.dedisp.dm_max         = 10.0F;
    cfg.dedisp.dm_tol         = 1.25F;
    cfg.dedisp.dm_pulse_width = 1000.0F;
    cfg.rfi.broad.enabled     = false;
    cfg.rfi.narrow.enabled    = false;
    cfg.search.boxcar_max     = 32;
    cfg.search.detect_thresh  = 0.5F;
    cfg.search.peak_mode      = dmt::PeakFindingMode::kAllAboveThreshold;
    cfg.search.cand_sep_time  = 1;
    cfg.search.cluster        = false;
    cfg.nsamps_gulp           = spec.nsamps;
    cfg.baseline_length       = 0.5F;
    cfg.output.output_dir     = (root / "out").string();
    cfg.output.per_gulp_files = false;
    frost::config::InputConfig input;
    input.sigproc_file = fil;
    auto source        = frost::pipeline::open_input(input);
    frost::pipeline::Pipeline pipeline(cfg, source);
    // The cap must actually have fired (and been reported), or this test
    // proves nothing about truncation.
    std::ostringstream warnings;
    const auto previous = spdlog::default_logger();
    auto capture        = std::make_shared<spdlog::logger>(
        "giant-cap", std::make_shared<spdlog::sinks::ostream_sink_mt>(
                         warnings, /*force_flush=*/true));
    capture->set_level(spdlog::level::warn);
    spdlog::set_default_logger(capture);
    bool threw = false;
    try {
        while (pipeline.execute() != 0) {
        }
    } catch (...) {
        threw = true;
    }
    spdlog::set_default_logger(previous);
    REQUIRE_FALSE(threw);
    CHECK(warnings.str().find("candidate capacity exceeded") !=
          std::string::npos);
    std::ifstream cand(root / "out" / "candidates.cand");
    REQUIRE(cand.is_open());
    std::size_t rows = 0;
    std::string line;
    while (std::getline(cand, line)) {
        ++rows;
    }
    CHECK(rows > 4096);
}
