// Runs the pipeline on a local J0540 recording (decimated, 6656
// channels, 512 us) with dispersed pulses injected at known DMs. Tagged [data];
// skipped when tests/cpp/data/benchmark_J0540_decimated.fil is absent; the
// recording is not distributed with frost.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <frost/config/config.hpp>
#include <frost/pipeline/input.hpp>
#include <frost/pipeline/pipeline.hpp>

#include "support/temp_dir.hpp"

#ifndef FROST_TEST_DATA_DIR
#define FROST_TEST_DATA_DIR ""
#endif

namespace {

constexpr double kDispersionK = 4148.808; // s MHz^2 / (pc cm^-3)

struct Injected {
    const char* name;
    double dm;
    std::size_t start_top;
    std::size_t width;
    float amp;
    std::size_t chan_lo;
    std::size_t chan_hi;
};

constexpr Injected kPulses[] = {
    {"A broadband DM 146.5", 146.5, 5000, 4, 7.0F, 0, 6655},
    {"B broadband DM 90", 90.0, 4500, 8, 4.0F, 0, 6655},
    {"C 1024 MHz DM 120", 120.0, 6000, 4, 14.0F, 4608, 6655},
};

struct Row {
    double snr;
    std::size_t samp;
    double dm;
    double f_low;
    double f_high;
};

std::filesystem::path data_file() {
    return std::filesystem::path(FROST_TEST_DATA_DIR) /
           "benchmark_J0540_decimated.fil";
}

// Copies the recording to `dst` with the pulses added in raw counts.
void inject(const std::filesystem::path& dst) {
    std::ifstream in(data_file(), std::ios::binary);
    std::vector<unsigned char> raw((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
    static const std::string kEnd = "HEADER_END";
    const auto it =
        std::search(raw.begin(), raw.end(), kEnd.begin(), kEnd.end());
    REQUIRE(it != raw.end());
    const auto offset =
        static_cast<std::size_t>(it - raw.begin()) + kEnd.size();

    frost::config::InputConfig input;
    input.sigproc_file       = data_file().string();
    const auto src           = frost::pipeline::open_input(input);
    const auto nch           = static_cast<std::size_t>(src.nchans());
    const double ts          = src.tsamp();
    const double fch1        = src.fch1();
    const double foff        = src.foff();
    const std::size_t nsamps = (raw.size() - offset) / nch;
    const double ftop =
        std::max(fch1, fch1 + (static_cast<double>(nch - 1) * foff));

    for (const auto& p : kPulses) {
        for (std::size_t c = p.chan_lo; c <= p.chan_hi && c < nch; ++c) {
            const double f   = fch1 + (static_cast<double>(c) * foff);
            const auto delay = static_cast<std::size_t>(
                std::llround(kDispersionK * p.dm *
                             ((1.0 / (f * f)) - (1.0 / (ftop * ftop))) / ts));
            const std::size_t t0 = p.start_top + delay;
            if (t0 + p.width > nsamps) {
                continue;
            }
            for (std::size_t t = t0; t < t0 + p.width; ++t) {
                auto& v = raw[offset + (t * nch) + c];
                v       = static_cast<unsigned char>(std::clamp(
                    std::round(static_cast<float>(v) + p.amp), 0.0F, 255.0F));
            }
        }
    }
    std::ofstream out(dst, std::ios::binary);
    out.write(reinterpret_cast<const char*>(raw.data()),
              static_cast<std::streamsize>(raw.size()));
}

std::vector<Row> run(const std::string& fil,
                     const std::filesystem::path& out_dir,
                     frost::SizeType levels) {
    frost::config::PipelineConfig cfg;
    cfg.input.sigproc_file = fil;
    cfg.dedisp.dm_max      = 250.0F;
    cfg.search.boxcar_max  = 32;
    // This recording needs channel-outlier rejection before analytical
    // normalization; production RFI defaults remain unchanged.
    cfg.rfi.moments.enabled = true;
    cfg.rfi.iqrm.enabled    = true;
    // The fixture includes real red noise; recovery must not depend on
    // parallel truncation at the production candidate capacity.
    cfg.search.max_candidates = 2'000'000;
    cfg.search.cand_sep_dm    = 10;
    cfg.search.subband_levels = levels;
    cfg.nsamps_gulp           = 8192;
    cfg.output.output_dir     = out_dir.string();
    cfg.output.per_gulp_files = false;

    auto source = frost::pipeline::open_input(cfg.input);
    frost::pipeline::Pipeline pipeline(cfg, source);
    while (pipeline.execute() != 0) {
    }

    std::vector<Row> rows;
    std::ifstream cand(out_dir / "candidates.cand");
    std::string line;
    while (std::getline(cand, line)) {
        std::istringstream is(line);
        double snr          = 0;
        double time         = 0;
        double dm           = 0;
        double f_low        = 0;
        double f_high       = 0;
        std::size_t samp    = 0;
        std::size_t filt    = 0;
        std::size_t dm_idx  = 0;
        std::size_t members = 0;
        std::size_t begin   = 0;
        std::size_t end     = 0;
        if (is >> snr >> samp >> time >> filt >> dm_idx >> dm >> members >>
            begin >> end >> f_low >> f_high) {
            rows.push_back({snr, samp, dm, f_low, f_high});
        }
    }
    return rows;
}

// Best row whose time agrees with the pulse's arrival at that row's own
// lowest frequency (FDMT timestamps each plane at its bottom edge) and whose
// DM is within 8 of the truth.
double best_snr(const std::vector<Row>& rows, const Injected& p, double ts) {
    const double ftop = 4031.75;
    double best       = 0.0;
    for (const auto& r : rows) {
        const double expect =
            static_cast<double>(p.start_top) +
            (kDispersionK * p.dm *
             ((1.0 / (r.f_low * r.f_low)) - (1.0 / (ftop * ftop))) / ts);
        if (std::abs(static_cast<double>(r.samp) - expect) <= 40.0 &&
            std::abs(r.dm - p.dm) <= 8.0) {
            best = std::max(best, r.snr);
        }
    }
    return best;
}

} // namespace

TEST_CASE("J0540 recording: injected dispersed pulses are recovered at every "
          "sub-band level",
          "[data][pipeline]") {
    if (!std::filesystem::exists(data_file())) {
        SKIP("benchmark_J0540_decimated.fil is not present");
    }
    const auto dir = frost::test::unique_temp_dir("real_data");
    const auto fil = (dir / "injected.fil").string();
    inject(fil);

    for (const frost::SizeType levels : {0U, 2U}) {
        CAPTURE(levels);
        const auto rows =
            run(fil, dir / ("out" + std::to_string(levels)), levels);
        REQUIRE_FALSE(rows.empty());
        for (const auto& p : kPulses) {
            CAPTURE(p.name);
            CHECK(best_snr(rows, p, 0.000512) > 15.0);
        }
    }
    std::filesystem::remove_all(dir);
}
