#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <fmt/format.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <frost/config/config.hpp>
#include <frost/dedisp/dedisp.hpp>
#include <frost/pipeline/input.hpp>
#include <frost/pipeline/pipeline.hpp>
#include <frost/prepare/rfi.hpp>

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

// Writes a synthetic 8-bit sigproc filterbank: a flat per-channel
// baseline plus a boxcar-shaped pulse injected identically across every
// channel at `pulse_start`, `pulse_width` samples wide.
std::string
make_filterbank_path(SizeType nchans,
                     SizeType nsamps,
                     SizeType pulse_start,
                     SizeType pulse_width,
                     uint8_t baseline,
                     uint8_t amplitude,
                     const std::string& name = "pipeline_test.fil") {
    const std::string path =
        (frost::test::unique_temp_dir("pipeline_fil") / name).string();
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
        if (t >= pulse_start && t < pulse_start + pulse_width) {
            std::fill(row.begin(), row.end(),
                      static_cast<uint8_t>(baseline + amplitude));
        } else {
            std::fill(row.begin(), row.end(), baseline);
        }
        os.write(reinterpret_cast<const char*>(row.data()),
                 static_cast<std::streamsize>(row.size()));
    }
    return path;
}

} // namespace

TEST_CASE("Pipeline runs a synthetic filterbank end to end and writes a "
          "candidate for an injected pulse",
          "[pipeline]") {
    constexpr SizeType kNChans     = 32;
    constexpr SizeType kNSamps     = 8192;
    constexpr SizeType kPulseStart = 4000;
    constexpr SizeType kPulseWidth = 4;

    const auto fil_path = make_filterbank_path(kNChans, kNSamps, kPulseStart,
                                               kPulseWidth, /*baseline=*/100,
                                               /*amplitude=*/60);

    frost::config::PipelineConfig cfg;
    cfg.input.sigproc_file = fil_path;
    // Isolate the boxcar/giant-finder/clustering wiring being tested here
    // from RFI cleaning, which is already covered by rfi_test.cpp.
    cfg.rfi.broad.enabled  = false;
    cfg.rfi.narrow.enabled = false;

    cfg.dedisp.dm_min         = 0.0F;
    cfg.dedisp.dm_max         = 50.0F;
    cfg.dedisp.dm_tol         = 1.25F;
    cfg.dedisp.dm_pulse_width = 1000.0F;

    cfg.search.boxcar_max      = 32;
    cfg.search.detect_thresh   = 5.0F;
    cfg.search.cand_sep_time   = 5;
    cfg.search.cand_sep_filter = 2;
    // An undispersed pulse lands on every low-DM trial. A generous
    // cand_sep_dm merges those per-trial detections into one candidate.
    cfg.search.cand_sep_dm       = 1'000'000;
    cfg.search.cand_sep_dm_smear = 0.0F; // trial-count merge only
    cfg.search.max_giant_rate    = 0.0F;

    cfg.nsamps_gulp       = kNSamps;
    cfg.baseline_length   = 1.0F;
    const auto out_dir    = frost::test::unique_temp_dir("pipeline_out");
    cfg.output.output_dir = out_dir.string();

    const std::string cand_path = (out_dir / "candidates.cand").string();

    {
        auto source = frost::pipeline::open_input(cfg.input);

        auto dedisperser = frost::dedisp::make_dmt_fdmt_plan(
            frost::pipeline::make_dedisp_plan_config(cfg, source));
        REQUIRE(kNSamps > dedisperser.get_dt_max() + cfg.search.boxcar_max);

        frost::pipeline::Pipeline pipeline(cfg, source);

        SizeType gulps = 0;
        while (pipeline.execute() != 0) {
            ++gulps;
        }
        CHECK(gulps == 1); // one full gulp; the run stops at EOF after it
    }

    std::ifstream cand_file(cand_path);
    REQUIRE(cand_file.is_open());

    std::string line;
    std::vector<std::string> lines;
    while (std::getline(cand_file, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    REQUIRE_FALSE(lines.empty());

    float best_snr           = -1.0F;
    SizeType best_samp_idx   = 0;
    SizeType best_filter_idx = 0;
    for (const auto& row : lines) {
        std::istringstream iss(row);
        float snr           = 0.0F;
        SizeType samp_idx   = 0;
        float time          = 0.0F;
        SizeType filter_idx = 0;
        iss >> snr >> samp_idx >> time >> filter_idx;
        if (snr > best_snr) {
            best_snr        = snr;
            best_samp_idx   = samp_idx;
            best_filter_idx = filter_idx;
        }
    }

    CHECK(best_snr > cfg.search.detect_thresh);
    // filter_idx = log2(filter_width); the injected pulse is 4 samples
    // wide, so the peak should land on filter_idx == 2 (width 4).
    CHECK(best_filter_idx == 2);
    // The peak should land within the injected pulse's time span.
    CHECK(best_samp_idx >= kPulseStart);
    CHECK(best_samp_idx < kPulseStart + kPulseWidth + cfg.search.boxcar_max);

    std::remove(fil_path.c_str());
    std::remove(cand_path.c_str());
}

TEST_CASE("FDMT pipeline merges peaks that close in different gulps",
          "[pipeline]") {
    constexpr SizeType kNChans = 32;
    constexpr SizeType kGulp   = 256;
    constexpr SizeType kNSamps = kGulp * 2;
    constexpr SizeType kBoxcar = 32;
    // Narrow peaks from pulse A close in gulp 1, wider peaks in gulp 2. Pulse B
    // is far enough into gulp 2 that the widest boxcar cannot cluster the two
    // (cand_sep_time scales with filter width, up to boxcar_max).
    constexpr SizeType kPulseA = kGulp - (kBoxcar / 2);
    constexpr SizeType kPulseB = 400;

    const auto fil_path = make_filterbank_path(
        kNChans, kNSamps, kPulseA, /*pulse_width=*/4, /*baseline=*/100,
        /*amplitude=*/60, "pipeline_boundary.fil");
    // Second pulse, written by re-opening would be messy. Inject both by
    // rebuilding: the helper writes one pulse. Overlay pulse B below.
    {
        std::fstream os(fil_path,
                        std::ios::binary | std::ios::in | std::ios::out);
        REQUIRE(os);
        os.seekg(0, std::ios::end);
        const auto file_end   = os.tellg();
        const auto data_bytes = static_cast<std::streamoff>(kNSamps * kNChans);
        const auto data_start = file_end - data_bytes;
        std::vector<uint8_t> row(kNChans, static_cast<uint8_t>(100 + 60));
        os.seekp(data_start + static_cast<std::streamoff>(kPulseB * kNChans));
        for (SizeType t = 0; t < 4; ++t) {
            os.write(reinterpret_cast<const char*>(row.data()),
                     static_cast<std::streamsize>(row.size()));
        }
    }

    frost::config::PipelineConfig cfg;
    cfg.input.sigproc_file       = fil_path;
    cfg.rfi.broad.enabled        = false;
    cfg.rfi.narrow.enabled       = false;
    cfg.dedisp.dm_min            = 0.0F;
    cfg.dedisp.dm_max            = 50.0F;
    cfg.dedisp.dm_tol            = 1.25F;
    cfg.dedisp.dm_pulse_width    = 1000.0F;
    cfg.search.boxcar_max        = kBoxcar;
    cfg.search.detect_thresh     = 5.0F;
    cfg.search.cand_sep_time     = 3;
    cfg.search.cand_sep_filter   = 2;
    cfg.search.cand_sep_dm       = 1'000'000;
    cfg.search.cand_sep_dm_smear = 0.0F; // trial-count merge only
    cfg.search.max_giant_rate    = 0.0F;
    cfg.nsamps_gulp              = kGulp;
    cfg.baseline_length          = 0.05F;
    const auto out_dir           = frost::test::unique_temp_dir("pipeline_out");
    cfg.output.output_dir        = out_dir.string();

    const std::string cand_path = (out_dir / "candidates.cand").string();

    SizeType searched = 0;
    {
        auto source      = frost::pipeline::open_input(cfg.input);
        auto dedisperser = frost::dedisp::make_dmt_fdmt_plan(
            frost::pipeline::make_dedisp_plan_config(cfg, source));
        REQUIRE(dedisperser.get_dt_max() > 0);
        REQUIRE(kGulp > dedisperser.get_dt_max() + cfg.search.boxcar_max);

        frost::pipeline::Pipeline pipeline(cfg, source);
        SizeType gulps = 0;
        while (pipeline.execute() != 0) {
            ++gulps;
        }
        CHECK(gulps == 2);
        searched = pipeline.nsamps_processed();
        CHECK(searched == kNSamps - dedisperser.get_dt_max());
    }

    std::ifstream cand_file(cand_path);
    REQUIRE(cand_file.is_open());
    std::string line;
    std::vector<SizeType> times;
    while (std::getline(cand_file, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream iss(line);
        float snr         = 0.0F;
        SizeType samp_idx = 0;
        float time        = 0.0F;
        iss >> snr >> samp_idx >> time;
        if (snr > cfg.search.detect_thresh) {
            times.push_back(samp_idx);
        }
    }

    const auto near = [&](SizeType pulse) {
        SizeType n = 0;
        for (const auto t : times) {
            if (t + kBoxcar >= pulse && t <= pulse + 4 + kBoxcar) {
                ++n;
            }
        }
        return n;
    };
    CHECK(near(kPulseA) == 1);
    CHECK(near(kPulseB) == 1);

    std::remove(fil_path.c_str());
    std::remove(cand_path.c_str());
}

namespace {

struct ExtentRow {
    float snr           = 0.0F;
    float dm            = 0.0F;
    SizeType samp       = 0;
    SizeType chan_begin = 0;
    SizeType chan_end   = 0;
};

std::vector<ExtentRow> read_extent_rows(const std::filesystem::path& path) {
    std::vector<ExtentRow> rows;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream iss(line);
        ExtentRow row;
        float time       = 0.0F;
        float dm         = 0.0F;
        float f_low      = 0.0F;
        float f_high     = 0.0F;
        SizeType samp    = 0;
        SizeType filter  = 0;
        SizeType dm_idx  = 0;
        SizeType members = 0;
        SizeType begin   = 0;
        SizeType end     = 0;
        iss >> row.snr >> samp >> time >> filter >> dm_idx >> dm >> members >>
            begin >> end >> f_low >> f_high >> row.chan_begin >> row.chan_end;
        row.samp = samp;
        row.dm   = dm;
        if (iss) {
            rows.push_back(row);
        }
    }
    return rows;
}

std::filesystem::path run_to_dir(frost::config::PipelineConfig cfg,
                                 const std::string& fil,
                                 const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    cfg.output.output_dir            = dir.string();
    frost::config::InputConfig input = cfg.input;
    input.sigproc_file               = fil;
    auto source                      = frost::pipeline::open_input(input);
    auto dedisperser                 = frost::dedisp::make_dmt_fdmt_plan(
        frost::pipeline::make_dedisp_plan_config(cfg, source));
    frost::pipeline::Pipeline pipeline(cfg, source);
    while (pipeline.execute() != 0) {
    }
    return dir / "candidates.cand";
}

} // namespace

TEST_CASE("Pipeline writes every detection when clustering is off",
          "[pipeline]") {
    constexpr SizeType kNChans = 32;
    constexpr SizeType kNSamps = 4096;
    const auto fil             = make_filterbank_path(
        kNChans, kNSamps, /*pulse_start=*/2000, /*pulse_width=*/4,
        /*baseline=*/100, /*amplitude=*/60, "pipeline_nocluster.fil");

    frost::config::PipelineConfig cfg;
    cfg.input.sigproc_file       = fil;
    cfg.rfi.broad.enabled        = false;
    cfg.rfi.narrow.enabled       = false;
    cfg.dedisp.dm_min            = 0.0F;
    cfg.dedisp.dm_max            = 80.0F;
    cfg.dedisp.dm_tol            = 1.25F;
    cfg.dedisp.dm_pulse_width    = 1000.0F;
    cfg.search.boxcar_max        = 16;
    cfg.search.detect_thresh     = 5.0F;
    cfg.search.cand_sep_dm       = 1'000'000;
    cfg.search.cand_sep_dm_smear = 0.0F; // trial-count merge only
    cfg.search.max_giant_rate    = 0.0F;
    cfg.nsamps_gulp              = kNSamps;
    cfg.baseline_length          = 0.5F;

    const auto root    = frost::test::unique_temp_dir("nocluster");
    cfg.search.cluster = true;
    const auto clustered =
        read_extent_rows(run_to_dir(cfg, fil, root / "clustered"));
    cfg.search.cluster = false;
    const auto raw     = read_extent_rows(run_to_dir(cfg, fil, root / "raw"));

    REQUIRE_FALSE(clustered.empty());
    REQUIRE(raw.size() > clustered.size());
    const auto span_of = [](const ExtentRow& row) {
        return row.chan_end - row.chan_begin + 1;
    };
    for (const auto& row : clustered) {
        CHECK(span_of(row) == kNChans);
    }
    for (const auto& row : raw) {
        CHECK(span_of(row) == kNChans);
    }

    std::filesystem::remove_all(root);
    std::filesystem::remove(fil);
}

TEST_CASE("trapezoid filters reach the fused search", "[pipeline]") {
    // A flat, sample-aligned width-8 pulse is matched exactly by the boxcar;
    // dmt's trapezoid (half-weight edges, same support) scores it about 5%
    // lower on the same analytical noise scale.
    constexpr SizeType kNChans = 32;
    constexpr SizeType kNSamps = 4096;
    const auto fil             = make_filterbank_path(
        kNChans, kNSamps, /*pulse_start=*/2000, /*pulse_width=*/8,
        /*baseline=*/100, /*amplitude=*/60, "pipeline_trapezoid.fil");

    frost::config::PipelineConfig cfg;
    cfg.input.sigproc_file    = fil;
    cfg.rfi.broad.enabled     = false;
    cfg.rfi.narrow.enabled    = false;
    cfg.dedisp.dm_min         = 0.0F;
    cfg.dedisp.dm_max         = 80.0F;
    cfg.dedisp.dm_tol         = 1.25F;
    cfg.dedisp.dm_pulse_width = 1000.0F;
    cfg.search.boxcar_max     = 16;
    cfg.search.wtsp           = 1.0F;
    cfg.search.detect_thresh  = 6.0F;
    cfg.search.max_giant_rate = 0.0F;
    cfg.nsamps_gulp           = kNSamps;
    cfg.baseline_length       = 0.5F;

    const auto root = frost::test::unique_temp_dir("trapezoid");
    const auto best = [](const std::filesystem::path& path) {
        const auto rows = read_extent_rows(path);
        REQUIRE_FALSE(rows.empty());
        return *std::ranges::max_element(
            rows, {}, [](const ExtentRow& row) { return row.snr; });
    };
    const auto boxcar        = best(run_to_dir(cfg, fil, root / "boxcar"));
    cfg.search.use_trapezoid = true;
    const auto trapezoid     = best(run_to_dir(cfg, fil, root / "trapezoid"));

    INFO("boxcar " << boxcar.snr << " trapezoid " << trapezoid.snr);
    CHECK(trapezoid.snr < boxcar.snr);
    CHECK(trapezoid.snr > 0.85F * boxcar.snr);
    CHECK(trapezoid.dm == boxcar.dm);

    std::filesystem::remove_all(root);
    std::filesystem::remove(fil);
}

TEST_CASE("Pipeline sub-band search collapses nested hits when clustering",
          "[pipeline]") {
    constexpr SizeType kNChans = 16;
    constexpr SizeType kNSamps = 4096;
    const auto fil             = make_filterbank_path(
        kNChans, kNSamps, /*pulse_start=*/2000, /*pulse_width=*/4,
        /*baseline=*/100, /*amplitude=*/80, "pipeline_subband.fil");

    frost::config::PipelineConfig cfg;
    cfg.input.sigproc_file       = fil;
    cfg.rfi.broad.enabled        = false;
    cfg.rfi.narrow.enabled       = false;
    cfg.dedisp.dm_min            = 0.0F;
    cfg.dedisp.dm_max            = 40.0F;
    cfg.dedisp.dm_tol            = 1.25F;
    cfg.dedisp.dm_pulse_width    = 1000.0F;
    cfg.search.boxcar_max        = 16;
    cfg.search.detect_thresh     = 6.0F;
    cfg.search.cand_sep_time     = 5;
    cfg.search.cand_sep_filter   = 3;
    cfg.search.cand_sep_dm       = 1'000'000;
    cfg.search.cand_sep_dm_smear = 0.0F; // trial-count merge only
    cfg.search.max_giant_rate    = 0.0F;
    cfg.search.subband_levels    = 2;
    cfg.nsamps_gulp              = kNSamps;
    cfg.baseline_length          = 0.5F;

    const auto root = frost::test::unique_temp_dir("subband_search");

    const auto span_of = [](const ExtentRow& row) {
        return row.chan_end - row.chan_begin + 1;
    };
    const auto has_span = [&](const std::vector<ExtentRow>& rows, bool narrow) {
        for (const auto& row : rows) {
            const SizeType span = span_of(row);
            if (narrow) {
                if (span > 0 && span < kNChans) {
                    return true;
                }
            } else if (span == kNChans) {
                return true;
            }
        }
        return false;
    };

    cfg.search.cluster = true;
    const auto clustered =
        read_extent_rows(run_to_dir(cfg, fil, root / "clustered"));
    REQUIRE_FALSE(clustered.empty());
    CHECK(has_span(clustered, /*narrow=*/false));
    CHECK_FALSE(has_span(clustered, /*narrow=*/true));

    cfg.search.cluster = false;
    const auto raw     = read_extent_rows(run_to_dir(cfg, fil, root / "raw"));
    CHECK(has_span(raw, /*narrow=*/false));
    CHECK(has_span(raw, /*narrow=*/true));

    std::filesystem::remove_all(root);
    std::filesystem::remove(fil);
}

TEST_CASE("Pipeline finds a pulse across the half-band boundary",
          "[pipeline][subband]") {
    const auto root = frost::test::unique_temp_dir("staggered_subband");
    frost::testing::FakeFilterbankSpec spec;
    spec.nchans      = 16;
    spec.nsamps      = 4096;
    spec.noise_sigma = 5.0F;
    const frost::testing::InjectedPulse pulse{.start      = 2000,
                                              .width      = 4,
                                              .amplitude  = 30.0F,
                                              .chan_start = 4,
                                              .chan_end   = 11};
    const auto fil = frost::testing::write_fake_filterbank(
        (root / "boundary.fil").string(), spec, {pulse});
    frost::config::PipelineConfig cfg;
    cfg.rfi.broad.enabled     = false;
    cfg.rfi.narrow.enabled    = false;
    cfg.dedisp.dm_max         = 10.0F;
    cfg.search.boxcar_max     = 16;
    cfg.search.max_giant_rate = 0.0F;
    cfg.search.cluster        = false;
    cfg.search.subband_levels = 1;
    cfg.nsamps_gulp           = spec.nsamps;
    const auto rows = read_extent_rows(run_to_dir(cfg, fil, root / "out"));
    // Only the staggered half spans original channels [4, 11]. Tree halves
    // split this pulse at channel 8; no output format change is required.
    CHECK(std::any_of(rows.begin(), rows.end(), [](const ExtentRow& row) {
        return row.chan_begin == 4 && row.chan_end == 11;
    }));
    std::filesystem::remove_all(root);
}

TEST_CASE("Pipeline merges a dispersed sub-band pulse onto the full band",
          "[pipeline]") {
    constexpr SizeType kNChans = 64;
    constexpr SizeType kNSamps = 4096;
    constexpr float kDm        = 200.0F;

    frost::testing::FakeFilterbankSpec spec;
    spec.nchans      = kNChans;
    spec.nsamps      = kNSamps;
    spec.tsamp       = 0.001;
    spec.fch1        = 1400.0;
    spec.foff        = -4.0;
    spec.baseline    = 100;
    spec.noise_sigma = 0.0F;

    frost::testing::InjectedPulse pulse;
    pulse.start     = 2000;
    pulse.width     = 4;
    pulse.amplitude = 80.0F;
    pulse.dm        = kDm;

    const auto fil = frost::testing::write_fake_filterbank(
        "pipeline_subband_dm.fil", spec, {pulse});

    frost::config::PipelineConfig cfg;
    cfg.input.sigproc_file       = fil;
    cfg.rfi.broad.enabled        = false;
    cfg.rfi.narrow.enabled       = false;
    cfg.dedisp.dm_min            = 0.0F;
    cfg.dedisp.dm_max            = 400.0F;
    cfg.dedisp.dm_tol            = 1.25F;
    cfg.dedisp.dm_pulse_width    = 1000.0F;
    cfg.search.boxcar_max        = 16;
    cfg.search.detect_thresh     = 6.0F;
    cfg.search.cand_sep_time     = 3;
    cfg.search.cand_sep_filter   = 3;
    cfg.search.cand_sep_dm       = 1'000'000;
    cfg.search.cand_sep_dm_smear = 0.0F; // trial-count merge only
    cfg.search.max_giant_rate    = 0.0F;
    cfg.search.subband_levels    = 2;
    cfg.nsamps_gulp              = kNSamps;
    cfg.baseline_length          = 0.5F;

    {
        frost::config::InputConfig input;
        input.sigproc_file = fil;
        auto source        = frost::pipeline::open_input(input);
        auto dedisperser   = frost::dedisp::make_dmt_fdmt_plan(
            frost::pipeline::make_dedisp_plan_config(cfg, source));
        REQUIRE(kNSamps > dedisperser.get_dt_max() + cfg.search.boxcar_max);
    }

    const auto root = frost::test::unique_temp_dir("subband_dm_curve");

    // Wide enough to cover the top-channel arrival and the full-band
    // bottom-edge arrival. A missing dispersion shift leaves the
    // earlier sub-band row inside this window.
    const auto in_window = [](const ExtentRow& row) {
        return row.samp >= 1800 && row.samp <= 2600 &&
               std::abs(row.dm - kDm) < 30.0F;
    };
    const auto span_of = [](const ExtentRow& row) {
        return row.chan_end - row.chan_begin + 1;
    };

    cfg.search.cluster = true;
    const auto clustered =
        read_extent_rows(run_to_dir(cfg, fil, root / "clustered"));
    SizeType full_in_window   = 0;
    SizeType narrow_in_window = 0;
    for (const auto& row : clustered) {
        if (!in_window(row)) {
            continue;
        }
        if (span_of(row) == kNChans) {
            ++full_in_window;
        } else if (span_of(row) > 0 && span_of(row) < kNChans) {
            ++narrow_in_window;
        }
    }
    CHECK(full_in_window == 1);
    CHECK(narrow_in_window == 0);

    cfg.search.cluster = false;
    const auto raw     = read_extent_rows(run_to_dir(cfg, fil, root / "raw"));
    bool raw_full      = false;
    bool raw_narrow    = false;
    for (const auto& row : raw) {
        if (!in_window(row)) {
            continue;
        }
        if (span_of(row) == kNChans) {
            raw_full = true;
        } else if (span_of(row) > 0 && span_of(row) < kNChans) {
            raw_narrow = true;
        }
    }
    CHECK(raw_full);
    CHECK(raw_narrow);

    std::filesystem::remove_all(root);
    std::filesystem::remove(fil);
}

TEST_CASE(
    "Pipeline sub-band extents follow balanced non-power-of-two FDMT nodes",
    "[pipeline]") {
    constexpr SizeType kNChans = 24;
    constexpr SizeType kNSamps = 4096;
    constexpr float kDm        = 40.0F;

    frost::testing::FakeFilterbankSpec spec;
    spec.nchans      = kNChans;
    spec.nsamps      = kNSamps;
    spec.tsamp       = 0.001;
    spec.fch1        = 1400.0;
    spec.foff        = -1.0;
    spec.baseline    = 100;
    spec.noise_sigma = 0.0F;

    frost::testing::InjectedPulse pulse;
    pulse.start     = 2000;
    pulse.width     = 4;
    pulse.amplitude = 80.0F;
    pulse.dm        = kDm;

    const auto fil = frost::testing::write_fake_filterbank(
        "pipeline_uneven_subband.fil", spec, {pulse});

    frost::config::PipelineConfig cfg;
    cfg.input.sigproc_file       = fil;
    cfg.rfi.broad.enabled        = false;
    cfg.rfi.narrow.enabled       = false;
    cfg.dedisp.dm_min            = 0.0F;
    cfg.dedisp.dm_max            = 120.0F;
    cfg.dedisp.dm_tol            = 1.25F;
    cfg.dedisp.dm_pulse_width    = 1000.0F;
    cfg.search.boxcar_max        = 16;
    cfg.search.detect_thresh     = 6.0F;
    cfg.search.cand_sep_time     = 5;
    cfg.search.cand_sep_filter   = 3;
    cfg.search.cand_sep_dm       = 1'000'000;
    cfg.search.cand_sep_dm_smear = 0.0F; // trial-count merge only
    cfg.search.max_giant_rate    = 0.0F;
    cfg.nsamps_gulp              = kNSamps;
    cfg.baseline_length          = 0.5F;

    const auto root = frost::test::unique_temp_dir("uneven_subband");

    const auto near_pulse = [](const ExtentRow& row) {
        return row.samp >= 1800 && row.samp <= 2600;
    };
    const auto dm_near = [](const ExtentRow& row) {
        return std::abs(row.dm - kDm) < 30.0F;
    };
    const auto span_of = [](const ExtentRow& row) {
        return row.chan_end - row.chan_begin + 1;
    };
    const auto has_channels = [&](const std::vector<ExtentRow>& rows,
                                  SizeType begin, SizeType end,
                                  bool dm_window) {
        for (const auto& row : rows) {
            if (!near_pulse(row) || row.chan_begin != begin ||
                row.chan_end != end) {
                continue;
            }
            if (!dm_window || dm_near(row)) {
                return true;
            }
        }
        return false;
    };

    cfg.search.subband_levels = 0;
    cfg.search.cluster        = true;
    const auto full_only =
        read_extent_rows(run_to_dir(cfg, fil, root / "full"));
    CHECK(has_channels(full_only, 0, kNChans - 1, /*dm_window=*/true));

    // The new top-down tree balances 24 channels into two halves of 12
    // and four quarters of 6, plus staggered bands across their boundaries.
    // Local DM grids are coarse, so match narrow rows in time only.
    cfg.search.subband_levels = 2;
    const auto clustered =
        read_extent_rows(run_to_dir(cfg, fil, root / "clustered"));
    CHECK(has_channels(clustered, 0, kNChans - 1, /*dm_window=*/true));
    for (const auto& row : clustered) {
        if (!near_pulse(row)) {
            continue;
        }
        CHECK(row.chan_begin == 0);
        CHECK(row.chan_end == kNChans - 1);
    }

    cfg.search.cluster = false;
    const auto raw     = read_extent_rows(run_to_dir(cfg, fil, root / "raw"));
    CHECK(has_channels(raw, 0, kNChans - 1, /*dm_window=*/true));
    for (const auto begin : {0U, 6U, 12U, 18U}) {
        CHECK(has_channels(raw, begin, begin + 5, /*dm_window=*/false));
    }
    for (const auto begin : {0U, 6U, 12U}) {
        CHECK(has_channels(raw, begin, begin + 11, /*dm_window=*/false));
    }
    for (const auto begin : {3U, 9U, 15U}) {
        CHECK(has_channels(raw, begin, begin + 5, /*dm_window=*/false));
    }
    for (const auto& row : raw) {
        if (!near_pulse(row)) {
            continue;
        }
        const SizeType span = span_of(row);
        CHECK((span == 6 || span == 12 || span == kNChans));
    }

    std::filesystem::remove_all(root);
    std::filesystem::remove(fil);
}

namespace {

// Default CLI verbosity is warnings. Capture that level so an info-only
// "max delay" line cannot satisfy the test.
class WarnCapture {
public:
    WarnCapture() {
        previous_ = spdlog::default_logger();
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(
            stream_, /*force_flush=*/true);
        logger_ = std::make_shared<spdlog::logger>("cold-prefix", sink);
        logger_->set_level(spdlog::level::warn);
        logger_->set_pattern("%v");
        spdlog::set_default_logger(logger_);
    }

    ~WarnCapture() {
        spdlog::set_default_logger(previous_);
        logger_.reset();
    }

    WarnCapture(const WarnCapture&)            = delete;
    WarnCapture& operator=(const WarnCapture&) = delete;

    std::string text() {
        logger_->flush();
        return stream_.str();
    }

private:
    std::ostringstream stream_;
    std::shared_ptr<spdlog::logger> logger_;
    std::shared_ptr<spdlog::logger> previous_;
};

struct ColdPrefixRun {
    SizeType delay      = 0;
    SizeType searched   = 0;
    SizeType candidates = 0;
    std::string log;
    std::string cand;
};

// Low band and a coarse sample time, so dm_max 200 is a few hundred
// samples of cold prefix. The gulp stays larger than that prefix.
ColdPrefixRun run_cold_prefix_band(SizeType nsamps) {
    constexpr SizeType kNChans = 16;
    constexpr SizeType kGulp   = 2048;

    frost::testing::FakeFilterbankSpec spec;
    spec.nchans      = kNChans;
    spec.nsamps      = nsamps;
    spec.tsamp       = 0.01;
    spec.fch1        = 200.0;
    spec.foff        = -1.0;
    spec.baseline    = 100;
    spec.noise_sigma = 0.0F;

    const auto root = frost::test::unique_temp_dir("cold_prefix");
    std::filesystem::create_directories(root);
    struct RemoveTree {
        std::filesystem::path path;
        ~RemoveTree() { std::filesystem::remove_all(path); }
    } cleanup{root};
    const auto fil = frost::testing::write_fake_filterbank(
        (root / "cold.fil").string(), spec, {});

    frost::config::PipelineConfig cfg;
    cfg.input.sigproc_file    = fil;
    cfg.rfi.broad.enabled     = false;
    cfg.rfi.narrow.enabled    = false;
    cfg.dedisp.dm_min         = 0.0F;
    cfg.dedisp.dm_max         = 200.0F;
    cfg.dedisp.dm_tol         = 1.25F;
    cfg.dedisp.dm_pulse_width = 10000.0F;
    cfg.search.boxcar_max     = 16;
    cfg.search.detect_thresh  = 6.0F;
    cfg.search.max_giant_rate = 0.0F;
    cfg.nsamps_gulp           = kGulp;
    cfg.baseline_length       = 0.5F;
    cfg.output.output_dir     = root.string();

    ColdPrefixRun out;
    {
        WarnCapture capture;
        auto source      = frost::pipeline::open_input(cfg.input);
        auto dedisperser = frost::dedisp::make_dmt_fdmt_plan(
            frost::pipeline::make_dedisp_plan_config(cfg, source));
        out.delay = dedisperser.get_dt_max();
        REQUIRE(kGulp > out.delay + cfg.search.boxcar_max);

        frost::pipeline::Pipeline pipeline(cfg, source);
        while (pipeline.execute() != 0) {
        }
        out.searched   = pipeline.nsamps_processed();
        out.candidates = pipeline.profile().candidates;
        CHECK(pipeline.profile().samples_read == nsamps);
        CHECK(pipeline.profile().samples_searched == out.searched);
        out.log = capture.text();
    }

    std::ifstream cand(root / "candidates.cand");
    REQUIRE(cand.is_open());
    out.cand.assign(std::istreambuf_iterator<char>(cand),
                    std::istreambuf_iterator<char>());
    return out;
}

} // namespace

TEST_CASE("Pipeline warns when the cold prefix covers the file", "[pipeline]") {
    constexpr SizeType kNSamps = 64;
    const auto run             = run_cold_prefix_band(kNSamps);
    REQUIRE(run.delay >= kNSamps);
    CHECK(run.searched == 0);
    CHECK(run.candidates == 0);
    CHECK(run.cand.empty());

    const auto expected =
        fmt::format("Cold prefix of {} samples left nothing to search: read {} "
                    "sample(s), searched {}",
                    run.delay, kNSamps, SizeType{0});
    CHECK(run.log.find(expected) != std::string::npos);
}

TEST_CASE("Pipeline does not warn when samples remain after the cold prefix",
          "[pipeline]") {
    constexpr SizeType kNSamps = 2048;
    const auto run             = run_cold_prefix_band(kNSamps);
    REQUIRE(run.delay > 0);
    REQUIRE(run.delay < kNSamps);
    CHECK(run.searched == kNSamps - run.delay);
    CHECK(run.log.find("Cold prefix") == std::string::npos);
    CHECK(run.log.find("left nothing to search") == std::string::npos);
}

TEST_CASE("fswap dedisperses on the swapped frequency axis", "[pipeline]") {
    // File A is descending (1400 MHz first). File B holds the same samples
    // with every spectrum reversed under an ascending header, so reading B
    // with fswap hands over exactly A's channel order. The dispersion model
    // must follow the channel order the reader delivers: both runs give the
    // same candidates, and the pulse is found at its DM.
    constexpr SizeType kNChans = 128;
    constexpr SizeType kNSamps = 4096;
    constexpr float kDm        = 100.0F;

    frost::testing::FakeFilterbankSpec spec;
    spec.nchans      = kNChans;
    spec.nsamps      = kNSamps;
    spec.tsamp       = 0.001;
    spec.fch1        = 1400.0;
    spec.foff        = -4.0;
    spec.baseline    = 100;
    spec.noise_sigma = 6.0F;

    frost::testing::InjectedPulse pulse;
    pulse.start     = 1500;
    pulse.width     = 4;
    pulse.amplitude = 20.0F;
    pulse.dm        = kDm;

    const auto root  = frost::test::unique_temp_dir("pipeline_fswap");
    const auto fil_a = (root / "descending.fil").string();
    const auto fil_b = (root / "ascending_reversed.fil").string();
    frost::testing::write_fake_filterbank(fil_a, spec, {pulse});

    auto spec_b   = spec;
    spec_b.fch1   = spec.fch1 + (static_cast<double>(kNChans - 1) * spec.foff);
    spec_b.foff   = -spec.foff;
    spec_b.nsamps = 0; // header only; the reversed samples are appended
    frost::testing::write_fake_filterbank(fil_b, spec_b, {});
    {
        std::ifstream in(fil_a, std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
        REQUIRE(bytes.size() > kNSamps * kNChans);
        const auto data =
            bytes.end() - static_cast<std::ptrdiff_t>(kNSamps * kNChans);
        std::ofstream out(fil_b, std::ios::binary | std::ios::app);
        for (SizeType t = 0; t < kNSamps; ++t) {
            std::vector<char> row(
                data + static_cast<std::ptrdiff_t>(t * kNChans),
                data + static_cast<std::ptrdiff_t>((t + 1) * kNChans));
            std::reverse(row.begin(), row.end());
            out.write(row.data(), static_cast<std::streamsize>(row.size()));
        }
    }

    frost::config::PipelineConfig cfg;
    cfg.rfi.broad.enabled     = false;
    cfg.rfi.narrow.enabled    = false;
    cfg.dedisp.dm_min         = 0.0F;
    cfg.dedisp.dm_max         = 200.0F;
    cfg.dedisp.dm_tol         = 1.25F;
    cfg.dedisp.dm_pulse_width = 1000.0F;
    cfg.search.boxcar_max     = 16;
    cfg.search.detect_thresh  = 7.0F;
    cfg.search.max_giant_rate = 0.0F;
    cfg.nsamps_gulp           = kNSamps;
    cfg.baseline_length       = 0.5F;

    const auto read_all = [](const std::filesystem::path& path) {
        std::ifstream in(path);
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    };

    const auto cand_a = run_to_dir(cfg, fil_a, root / "a");
    cfg.input.fswap   = true;
    const auto cand_b = run_to_dir(cfg, fil_b, root / "b");

    const auto rows = read_extent_rows(cand_a);
    REQUIRE_FALSE(rows.empty());
    const auto best = std::ranges::max_element(
        rows, {}, [](const ExtentRow& row) { return row.snr; });
    CHECK(best->snr > 10.0F);
    CHECK(std::abs(best->dm - kDm) < 10.0F);
    CHECK(read_all(cand_b) == read_all(cand_a));

    std::filesystem::remove_all(root);
}

namespace {
constexpr SizeType kMaskChans = 128;

frost::testing::FakeFilterbankSpec mask_order_spec(SizeType nsamps) {
    frost::testing::FakeFilterbankSpec spec;
    spec.nchans      = kMaskChans;
    spec.nsamps      = nsamps;
    spec.tsamp       = 0.001;
    spec.fch1        = 1400.0;
    spec.foff        = -4.0;
    spec.baseline    = 100;
    spec.noise_sigma = 6.0F;
    return spec;
}

// Writes `fil_a` (descending, `spec`) after `edit` changes its 8-bit samples,
// and `fil_b`: the same spectra reversed under an ascending header. Returns
// A's samples.
template <typename Edit>
std::vector<char>
write_mask_order_pair(const frost::testing::FakeFilterbankSpec& spec,
                      const frost::testing::InjectedPulse& pulse,
                      const std::string& fil_a,
                      const std::string& fil_b,
                      Edit edit) {
    const auto n = static_cast<std::ptrdiff_t>(spec.nsamps * kMaskChans);
    frost::testing::write_fake_filterbank(fil_a, spec, {pulse});
    std::vector<char> bytes;
    {
        std::ifstream in(fil_a, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
    }
    REQUIRE(bytes.size() > static_cast<std::size_t>(n));
    std::vector<char> data(bytes.end() - n, bytes.end());
    edit(data);
    std::copy(data.begin(), data.end(), bytes.end() - n);
    std::ofstream(fil_a, std::ios::binary)
        .write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    auto spec_b = spec;
    spec_b.fch1 = spec.fch1 + (static_cast<double>(kMaskChans - 1) * spec.foff);
    spec_b.foff = -spec.foff;
    spec_b.nsamps = 0; // header only; the reversed samples are appended
    frost::testing::write_fake_filterbank(fil_b, spec_b, {});
    std::ofstream out(fil_b, std::ios::binary | std::ios::app);
    for (SizeType t = 0; t < spec.nsamps; ++t) {
        const auto row =
            data.begin() + static_cast<std::ptrdiff_t>(t * kMaskChans);
        std::vector<char> reversed(row, row + kMaskChans);
        std::reverse(reversed.begin(), reversed.end());
        out.write(reversed.data(), static_cast<std::streamsize>(kMaskChans));
    }
    return data;
}

frost::config::PipelineConfig mask_order_config(SizeType gulp) {
    frost::config::PipelineConfig cfg;
    cfg.rfi.broad.enabled     = false;
    cfg.rfi.narrow.enabled    = false;
    cfg.dedisp.dm_min         = 0.0F;
    cfg.dedisp.dm_max         = 200.0F;
    cfg.dedisp.dm_tol         = 1.25F;
    cfg.dedisp.dm_pulse_width = 1000.0F;
    cfg.search.boxcar_max     = 16;
    cfg.search.detect_thresh  = 7.0F;
    cfg.search.max_giant_rate = 0.0F;
    cfg.nsamps_gulp           = gulp;
    cfg.baseline_length       = 0.5F;
    return cfg;
}

ExtentRow strongest(const std::filesystem::path& path) {
    const auto rows = read_extent_rows(path);
    REQUIRE_FALSE(rows.empty());
    return *std::ranges::max_element(
        rows, {}, [](const ExtentRow& row) { return row.snr; });
}

// Frequency-order-independent candidate coordinates.
std::vector<std::tuple<SizeType, float, float>>
search_rows(const std::filesystem::path& path) {
    std::vector<std::tuple<SizeType, float, float>> out;
    for (const auto& row : read_extent_rows(path))
        out.emplace_back(row.samp, row.dm, row.snr);
    std::ranges::sort(out);
    return out;
}
} // namespace

TEST_CASE("static channel zaps reach dmt in its frequency order",
          "[pipeline][rfi]") {
    // The same spectra under descending (A) and ascending (B) headers,
    // with the low half of the band zapped by frequency (the engine's
    // constructor mask). dmt sees identical input and masks only if the mask
    // is reversed with the data for A; with the masked channels left out of
    // its noise model, the half-band pulse scores about 1/sqrt(2) of its
    // full-band S/N.
    constexpr SizeType kNSamps = 4096;
    frost::testing::InjectedPulse pulse;
    pulse.start     = 1500;
    pulse.width     = 4;
    pulse.amplitude = 20.0F;
    pulse.dm        = 100.0F;

    const auto root  = frost::test::unique_temp_dir("pipeline_zap_order");
    const auto fil_a = (root / "descending.fil").string();
    const auto fil_b = (root / "ascending.fil").string();
    write_mask_order_pair(mask_order_spec(kNSamps), pulse, fil_a, fil_b,
                          [](std::vector<char>&) {});

    auto cfg          = mask_order_config(kNSamps);
    const auto full   = strongest(run_to_dir(cfg, fil_a, root / "full"));
    cfg.rfi.zap_freqs = {{.lo = 880.0, .hi = 1145.0}};
    const auto a      = strongest(run_to_dir(cfg, fil_a, root / "a"));
    const auto b      = strongest(run_to_dir(cfg, fil_b, root / "b"));

    CHECK(a.snr == b.snr);
    CHECK(a.dm == b.dm);
    CHECK(a.samp == b.samp);
    CHECK(std::abs(a.dm - pulse.dm) < 10.0F);
    const double ratio = a.snr / full.snr;
    INFO("full " << full.snr << " half " << a.snr);
    CHECK(ratio > 0.55);
    CHECK(ratio < 0.85);

    std::filesystem::remove_all(root);
}

TEST_CASE("per-gulp RFI masks reach dmt in its frequency order",
          "[pipeline][rfi][streaming]") {
    // Dead (constant) channels differ in every gulp: none, then ten
    // high-frequency channels, then ten low-frequency channels crossed by
    // the pulse. These per-call masks change across gulp boundaries and must
    // be reversed with the data on the descending file for both runs to
    // hand dmt identical input and masks.
    constexpr SizeType kGulp   = 2048;
    constexpr SizeType kNSamps = 3 * kGulp;
    constexpr SizeType kDead   = 10;
    frost::testing::InjectedPulse pulse;
    pulse.start     = 4300;
    pulse.width     = 4;
    pulse.amplitude = 20.0F;
    pulse.dm        = 100.0F;

    const auto root  = frost::test::unique_temp_dir("pipeline_gulp_masks");
    const auto fil_a = (root / "descending.fil").string();
    const auto fil_b = (root / "ascending.fil").string();
    const auto kill  = [](std::vector<char>& data, SizeType gulp,
                          SizeType first) {
        for (SizeType t = gulp * kGulp; t < (gulp + 1) * kGulp; ++t) {
            for (SizeType c = first; c < first + kDead; ++c)
                data[(t * kMaskChans) + c] = 100;
        }
    };
    auto data = write_mask_order_pair(mask_order_spec(kNSamps), pulse, fil_a,
                                      fil_b, [&](std::vector<char>& d) {
                                          kill(d, 1, 10);
                                          kill(d, 2, 100);
                                      });

    const auto cfg = mask_order_config(kGulp);
    // The RFI stage itself masks a different set of channels in each gulp.
    frost::prepare::rfi::RFICleaner cleaner(cfg.rfi,
                                            {.nchans     = kMaskChans,
                                             .max_nsamps = kGulp,
                                             .dtype = frost::DataType::kUInt8,
                                             .tsamp = 0.001,
                                             .f0    = 1400.0,
                                             .df    = -4.0},
                                            cfg.baseline_length);
    for (SizeType g = 0; g < 3; ++g) {
        std::vector<uint8_t> block(
            data.begin() + static_cast<std::ptrdiff_t>(g * kGulp * kMaskChans),
            data.begin() +
                static_cast<std::ptrdiff_t>((g + 1) * kGulp * kMaskChans));
        (void)cleaner.execute_normalized(block);
        CHECK(cleaner.report().chans_zapped == 0);
        CHECK(cleaner.report().chans_masked == (g == 0 ? 0 : kDead));
    }

    const auto a = search_rows(run_to_dir(cfg, fil_a, root / "a"));
    const auto b = search_rows(run_to_dir(cfg, fil_b, root / "b"));
    REQUIRE_FALSE(a.empty());
    CHECK(a == b);
    const auto best = strongest(root / "a" / "candidates.cand");
    CHECK(best.snr > 10.0F);
    CHECK(std::abs(best.dm - pulse.dm) < 10.0F);

    std::filesystem::remove_all(root);
}

TEST_CASE("fused search warns once on candidate truncation", "[pipeline]") {
    const auto dir = frost::test::unique_temp_dir("capacity");
    frost::testing::FakeFilterbankSpec spec;
    spec.nchans      = 16;
    spec.nsamps      = 2048;
    spec.noise_sigma = 5.0F;
    const auto fil   = frost::testing::write_fake_filterbank(
        (dir / "noise.fil").string(), spec, {});
    frost::config::PipelineConfig cfg;
    cfg.nsamps_gulp             = 512;
    cfg.dedisp.dm_max           = 10.0F;
    cfg.search.boxcar_max       = 8;
    cfg.search.detect_thresh    = 0.1F;
    cfg.search.cluster          = false;
    cfg.search.peak_mode        = dmt::PeakFindingMode::kAllAboveThreshold;
    cfg.search.max_candidates   = 2;
    cfg.search.max_cands_per_dm = 1;
    cfg.rfi.narrow.enabled = cfg.rfi.broad.enabled = false;
    WarnCapture capture;
    const auto path     = run_to_dir(cfg, fil, dir / "out");
    const auto warnings = capture.text();
    const auto first    = warnings.find("candidate capacity exceeded");
    REQUIRE(first != std::string::npos);
    CHECK(warnings.find("candidate capacity exceeded", first + 1) ==
          std::string::npos);
    const auto rows = read_extent_rows(path);
    REQUIRE_FALSE(rows.empty());
    CHECK(rows.size() <= 8);
}

TEST_CASE("pipeline flushes a peak at the exact final sample once",
          "[pipeline]") {
    const auto dir = frost::test::unique_temp_dir("flush_peak");
    frost::testing::FakeFilterbankSpec spec;
    spec.nchans    = 16;
    spec.nsamps    = 512;
    const auto fil = frost::testing::write_fake_filterbank(
        (dir / "last.fil").string(), spec, {{511, 1, 60.0F, 0.0F}});
    frost::config::PipelineConfig cfg;
    cfg.nsamps_gulp        = 512;
    cfg.dedisp.dm_max      = 1.0F;
    cfg.search.boxcar_max  = 8;
    cfg.search.cluster     = false;
    cfg.rfi.narrow.enabled = cfg.rfi.broad.enabled = false;
    cfg.output.output_dir                          = (dir / "out").string();
    auto source_cfg                                = cfg.input;
    source_cfg.sigproc_file                        = fil;
    auto source = frost::pipeline::open_input(source_cfg);
    frost::pipeline::Pipeline pipeline(cfg, source);
    CHECK(pipeline.execute() == 512);
    CHECK(pipeline.execute() == 0);
    const auto count = pipeline.profile().candidates;
    CHECK(pipeline.execute() == 0);
    CHECK(pipeline.profile().candidates == count);
    const auto rows = read_extent_rows(dir / "out" / "candidates.cand");
    REQUIRE_FALSE(rows.empty());
    CHECK(std::any_of(rows.begin(), rows.end(),
                      [](const auto& row) { return row.samp == 511; }));
    CHECK(std::all_of(rows.begin(), rows.end(),
                      [](const auto& row) { return row.samp <= 511; }));
}

TEST_CASE("Pipeline rejects non-finite float input before normalized search",
          "[pipeline][numeric][robust]") {
    constexpr SizeType kChans = 16, kSamples = 2048, kGulp = 512;
    const auto root = frost::test::unique_temp_dir("pipeline_nonfinite");
    struct RemoveTree {
        std::filesystem::path path;
        ~RemoveTree() { std::filesystem::remove_all(path); }
    } cleanup{root};
    std::mt19937 rng(1523);
    std::normal_distribution<float> noise(0.0F, 1.0F);
    std::vector<std::uint32_t> samples(kChans * kSamples);
    for (SizeType t = 0; t < kSamples; ++t) {
        for (SizeType c = 0; c < kChans; ++c) {
            float value = 100.0F + static_cast<float>(c) +
                          (2.0F + static_cast<float>(c)) * noise(rng);
            if (t >= 1000 && t < 1004)
                value += 120.0F;
            samples[t * kChans + c] = std::bit_cast<std::uint32_t>(value);
        }
    }
    const auto write_float_file = [&](const char* name) {
        const auto path = root / name;
        std::ofstream out(path, std::ios::binary);
        write_string_field(out, "HEADER_START");
        write_kv(out, "data_type", 1);
        write_kv(out, "nchans", static_cast<int>(kChans));
        write_kv(out, "nbits", 32);
        write_kv(out, "nifs", 1);
        write_kv(out, "fch1", 1400.0);
        write_kv(out, "foff", -1.0);
        write_kv(out, "tsamp", 0.001);
        write_kv(out, "tstart", 58000.0);
        write_string_field(out, "HEADER_END");
        out.write(reinterpret_cast<const char*>(samples.data()),
                  static_cast<std::streamsize>(samples.size() *
                                               sizeof(std::uint32_t)));
        return path.string();
    };
    const auto valid_file       = write_float_file("valid.fil");
    samples[kGulp * kChans + 2] = 0x7FC00000U;
    const auto damaged_file     = write_float_file("damaged.fil");
    frost::config::PipelineConfig cfg;
    cfg.nsamps_gulp          = kGulp;
    cfg.baseline_length      = 0.5F;
    cfg.dedisp.dm_max        = 20.0F;
    cfg.search.boxcar_max    = 16;
    cfg.search.detect_thresh = 7.0F;
    cfg.rfi.narrow.enabled = cfg.rfi.broad.enabled = false;
    const auto valid_path = run_to_dir(cfg, valid_file, root / "valid");
    REQUIRE_FALSE(read_extent_rows(valid_path).empty());
    CHECK_THROWS_WITH(run_to_dir(cfg, damaged_file, root / "damaged"),
                      "RFI: non-finite float32 input at sample 0, channel 2");
}
