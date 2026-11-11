#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <frost/config/config.hpp>

using frost::config::PipelineConfig;

namespace {

const char* kToml = R"(
nsamps_gulp = 4096
verbosity = 2
baseline_length = 0.5
async_io = false
profile = true

[input]
sigproc_file = "foo.fil"
dada_key = "0xdada"
fswap = true
beam = 2

[rfi]
zap_chans = [[0, 3], [100, 120]]
zap_freqs = [[1420.0, 1421.5]]
fill = "noise"
seed = 42
zero_dm = "zdot"
equalize = true
mask_grow = 2

[rfi.narrow]
enabled = true
threshold = 4.5

[rfi.broad]
enabled = false
threshold = 6.0
boxcar_max = 4

[rfi.dropout]
enabled = true
min_std_fraction = 0.2

[rfi.moments]
enabled = true
threshold = 3.0
skewness = false
kurtosis_max = 2.5

[rfi.iqrm]
enabled = true
threshold = 2.5
radius = 7

[rfi.kadane_channel]
enabled = true
threshold = 8.0
min_duration = 0.1

[rfi.kadane_spectrum]
enabled = true
threshold = 6.5
min_bandwidth = 5.0
whole_band_fraction = 0.9

[dedisp]
dm_min = 1.0
dm_max = 500.0
dm_tol = 1.1
dm_pulse_width = 50.0
dm_nbits = 16
device = "cpu"

[search]
boxcar_max = 64
peak_mode = "across_widths"
max_candidates = 2048
max_cands_per_dm = 64
detect_thresh = 7.5
cand_sep_time = 4
cand_sep_filter = 2
cand_sep_dm = 100
max_giant_rate = 1000.0
cluster = false
subband_levels = 2

[output]
output_dir = "/tmp/frost_out"
beam = 5
per_gulp_files = true
)";

} // namespace

TEST_CASE("PipelineConfig::from_toml loads every field", "[config]") {
    const std::string path = "config_test.toml";
    {
        std::ofstream out(path);
        out << kToml;
    }

    const auto cfg = PipelineConfig::from_toml(path);

    CHECK(cfg.nsamps_gulp == 4096);
    CHECK(cfg.baseline_length == 0.5F);

    CHECK(cfg.input.sigproc_file == "foo.fil");
    CHECK(cfg.input.dada_key == "0xdada");
    CHECK(cfg.input.fswap);
    CHECK(cfg.input.beam == 2);

    using frost::prepare::rfi::FillMode;
    using frost::prepare::rfi::ZeroDMMode;
    const auto& rfi = cfg.rfi;
    CHECK(rfi.narrow.enabled);
    CHECK(rfi.narrow.threshold == 4.5F);
    CHECK_FALSE(rfi.broad.enabled);
    CHECK(rfi.broad.threshold == 6.0F);
    CHECK(rfi.broad.boxcar_max == 4);
    REQUIRE(rfi.zap_freqs.size() == 1);
    CHECK(rfi.zap_freqs[0].lo == 1420.0);
    CHECK(rfi.zap_freqs[0].hi == 1421.5);
    CHECK(rfi.fill == FillMode::kNoise);
    CHECK(rfi.seed == 42);
    CHECK(rfi.zero_dm == ZeroDMMode::kZdot);
    CHECK(rfi.equalize);
    CHECK(rfi.mask_grow == 2);
    CHECK(rfi.dropout.enabled);
    CHECK(rfi.dropout.min_std_fraction == 0.2F);
    CHECK(rfi.moments.enabled);
    CHECK(rfi.moments.threshold == 3.0F);
    CHECK_FALSE(rfi.moments.skewness);
    CHECK(rfi.moments.kurtosis);
    CHECK(rfi.moments.kurtosis_max == 2.5F);
    CHECK(rfi.iqrm.enabled);
    CHECK(rfi.iqrm.threshold == 2.5F);
    CHECK(rfi.iqrm.radius == 7);
    CHECK(rfi.kadane_channel.enabled);
    CHECK(rfi.kadane_channel.threshold == 8.0F);
    CHECK(rfi.kadane_channel.min_duration == 0.1F);
    CHECK(rfi.kadane_spectrum.enabled);
    CHECK(rfi.kadane_spectrum.threshold == 6.5F);
    CHECK(rfi.kadane_spectrum.min_bandwidth == 5.0F);
    CHECK(rfi.kadane_spectrum.whole_band_fraction == 0.9F);

    CHECK(cfg.dedisp.dm_min == 1.0F);
    CHECK(cfg.dedisp.dm_max == 500.0F);
    CHECK(cfg.dedisp.dm_tol == 1.1F);
    CHECK(cfg.dedisp.dm_pulse_width == 50.0F);

    CHECK(cfg.search.boxcar_max == 64);
    CHECK(cfg.search.detect_thresh == 7.5F);
    CHECK(cfg.search.cand_sep_time == 4);
    CHECK(cfg.search.cand_sep_filter == 2);
    CHECK(cfg.search.cand_sep_dm == 100);
    CHECK(cfg.search.max_giant_rate == 1000.0F);
    CHECK_FALSE(cfg.search.cluster);
    CHECK(cfg.search.subband_levels == 2);

    CHECK(cfg.output.output_dir == "/tmp/frost_out");

    CHECK(cfg.verbosity == 2);
    CHECK_FALSE(cfg.async_io);
    CHECK(cfg.profile);
    REQUIRE(cfg.rfi.zap_chans.size() == 2);
    CHECK(cfg.rfi.zap_chans[0].start == 0);
    CHECK(cfg.rfi.zap_chans[0].end == 3);
    CHECK(cfg.rfi.zap_chans[1].start == 100);
    CHECK(cfg.rfi.zap_chans[1].end == 120);
    CHECK(cfg.dedisp.dm_dtype == frost::DataType::kUInt16);
    CHECK(cfg.dedisp.device == dmt::Backend::kCPU);
    CHECK(cfg.output.beam == 5);
    CHECK(cfg.output.per_gulp_files);

    std::remove(path.c_str());
}

TEST_CASE("PipelineConfig::from_toml keeps struct defaults for omitted "
          "fields",
          "[config]") {
    const std::string path = "config_test_minimal.toml";
    {
        std::ofstream out(path);
        out << "[input]\nsigproc_file = \"bar.fil\"\n";
    }

    const PipelineConfig defaults; // struct defaults, for comparison
    const auto cfg = PipelineConfig::from_toml(path);

    CHECK(cfg.input.sigproc_file == "bar.fil");
    CHECK(cfg.input.dada_key.empty());
    CHECK(cfg.input.beam == 0);
    CHECK(cfg.async_io);
    CHECK_FALSE(cfg.profile);
    CHECK(cfg.nsamps_gulp == defaults.nsamps_gulp);
    CHECK(cfg.rfi.narrow.threshold == defaults.rfi.narrow.threshold);
    CHECK(cfg.rfi.fill == defaults.rfi.fill);
    CHECK(cfg.search.boxcar_max == defaults.search.boxcar_max);

    std::remove(path.c_str());
}

TEST_CASE("PipelineConfig defaults are the documented search defaults",
          "[config]") {
    const PipelineConfig cfg;
    CHECK(cfg.nsamps_gulp == 262144);
    CHECK(cfg.baseline_length == 2.0F);
    CHECK(cfg.rfi.narrow.enabled);
    CHECK(cfg.rfi.broad.enabled);
    CHECK(cfg.rfi.narrow.threshold == 5.0F);
    CHECK(cfg.rfi.broad.threshold == 5.0F);
    CHECK(cfg.rfi.broad.boxcar_max == 1);
    CHECK(cfg.rfi.fill == frost::prepare::rfi::FillMode::kNeighbour);
    CHECK(cfg.rfi.zero_dm == frost::prepare::rfi::ZeroDMMode::kOff);
    CHECK_FALSE(cfg.rfi.moments.enabled);
    CHECK_FALSE(cfg.rfi.iqrm.enabled);
    CHECK_FALSE(cfg.rfi.equalize);
    CHECK(cfg.dedisp.dm_min == 0.0F);
    CHECK(cfg.dedisp.dm_max == 1000.0F);
    CHECK(cfg.dedisp.dm_tol == 1.25F);
    CHECK(cfg.dedisp.dm_pulse_width == 40.0F);
    CHECK(cfg.search.boxcar_max == 4096);
    CHECK(cfg.search.wtsp == 2.0F);
    CHECK(cfg.search.detect_thresh == 6.0F);
    CHECK(cfg.search.cand_sep_time == 3);
    CHECK(cfg.search.cand_sep_filter == 3);
    CHECK(cfg.search.cand_sep_dm == 200);
    CHECK(cfg.search.cluster);
    CHECK(cfg.search.subband_levels == 0);
    CHECK(cfg.dedisp.device == dmt::Backend::kCPU);
    CHECK(cfg.dedisp.device_id == 0);
    CHECK(cfg.input.dada_key.empty());
    CHECK(cfg.async_io);
    CHECK_FALSE(cfg.profile);
}

TEST_CASE("parse_dada_key accepts hex keys", "[config]") {
    using frost::config::parse_dada_key;
    CHECK(parse_dada_key("dada") == 0xdadaU);
    CHECK(parse_dada_key("0xDADA") == 0xdadaU);
    CHECK(parse_dada_key("0") == 0U);
    CHECK_THROWS_AS(parse_dada_key("zz"), std::invalid_argument);
    CHECK_THROWS_AS(parse_dada_key(""), std::invalid_argument);
}

TEST_CASE("PipelineConfig::from_toml rejects a bad PSRDADA key", "[config]") {
    const std::string path = "config_test_bad_dada.toml";
    {
        std::ofstream out(path);
        out << "[input]\ndada_key = \"zz\"\n";
    }
    CHECK_THROWS_AS(PipelineConfig::from_toml(path), std::invalid_argument);
    std::remove(path.c_str());
}

TEST_CASE("dm_dtype_from_nbits maps dm_nbits", "[config]") {
    using frost::DataType;
    using frost::config::dm_dtype_from_nbits;
    CHECK(dm_dtype_from_nbits(8) == DataType::kUInt8);
    CHECK(dm_dtype_from_nbits(16) == DataType::kUInt16);
    CHECK(dm_dtype_from_nbits(32) == DataType::kFloat32);
    CHECK_THROWS_AS(dm_dtype_from_nbits(4), std::invalid_argument);
}

TEST_CASE("PipelineConfig::from_toml parses the FDMT device", "[config]") {
    const std::string path = "config_test_device.toml";
    {
        std::ofstream out(path);
        out << "[dedisp]\ndevice = \"cuda\"\n";
    }
    const auto cfg = PipelineConfig::from_toml(path);
    CHECK(cfg.dedisp.device == dmt::Backend::kCUDA);
    CHECK(cfg.dedisp.device_id == 0);
    std::remove(path.c_str());

    {
        std::ofstream out(path);
        out << "[dedisp]\ndevice = \"hip\"\ndevice_id = 3\n";
    }
    const auto hip = PipelineConfig::from_toml(path);
    CHECK(hip.dedisp.device == dmt::Backend::kHIP);
    CHECK(hip.dedisp.device_id == 3);
    std::remove(path.c_str());

    {
        std::ofstream out(path);
        out << "[dedisp]\ndevice = \"cpu\"\ndevice_id = 0\n";
    }
    const auto cpu = PipelineConfig::from_toml(path);
    CHECK(cpu.dedisp.device == dmt::Backend::kCPU);
    CHECK(cpu.dedisp.device_id == 0);
    std::remove(path.c_str());

    {
        std::ofstream out(path);
        out << "[dedisp]\ndevice = \"gpu\"\n";
    }
    CHECK_THROWS_AS(PipelineConfig::from_toml(path), std::invalid_argument);
    std::remove(path.c_str());

    {
        std::ofstream out(path);
        out << "[dedisp]\ndevice_id = -1\n";
    }
    CHECK_THROWS_AS(PipelineConfig::from_toml(path), std::invalid_argument);
    std::remove(path.c_str());
}

TEST_CASE("PipelineConfig::from_toml rejects subband_levels above 2",
          "[config]") {
    const std::string path = "config_test_bad_subband.toml";
    {
        std::ofstream out(path);
        out << "[search]\nsubband_levels = 3\n";
    }
    CHECK_THROWS_AS(PipelineConfig::from_toml(path), std::invalid_argument);
    std::remove(path.c_str());
}

TEST_CASE("PipelineConfig::from_toml rejects malformed zap_chans", "[config]") {
    const std::string path = "config_test_bad_zap.toml";
    for (const char* body :
         {"[rfi]\nzap_chans = [[5]]\n", "[rfi]\nzap_chans = [[9, 3]]\n"}) {
        {
            std::ofstream out(path);
            out << body;
        }
        CHECK_THROWS_AS(PipelineConfig::from_toml(path), std::invalid_argument);
    }
    std::remove(path.c_str());
}

TEST_CASE("PipelineConfig::from_toml rejects malformed zap_freqs and unknown "
          "RFI modes",
          "[config]") {
    const std::string path = "config_test_bad_rfi.toml";
    for (const char* body :
         {"[rfi]\nzap_freqs = [[1400.0]]\n",
          "[rfi]\nzap_freqs = [[1401.0, 1400.0]]\n", "[rfi]\nfill = \"zero\"\n",
          "[rfi]\nzero_dm = \"yes\"\n"}) {
        {
            std::ofstream out(path);
            out << body;
        }
        CHECK_THROWS_AS(PipelineConfig::from_toml(path), std::invalid_argument);
    }
    std::remove(path.c_str());
}

TEST_CASE("RFI enum names round-trip", "[config]") {
    using frost::prepare::rfi::FillMode;
    using frost::prepare::rfi::ZeroDMMode;
    for (const auto m :
         {FillMode::kNeighbour, FillMode::kMean, FillMode::kNoise}) {
        CHECK(frost::config::fill_mode_from_string(
                  frost::config::to_string(m)) == m);
    }
    for (const auto m :
         {ZeroDMMode::kOff, ZeroDMMode::kSubtract, ZeroDMMode::kZdot}) {
        CHECK(frost::config::zero_dm_from_string(frost::config::to_string(m)) ==
              m);
    }
}

TEST_CASE("search peak modes and capacities are validated", "[config]") {
    using frost::config::peak_mode_from_string;
    CHECK(peak_mode_from_string("per_width") ==
          dmt::PeakFindingMode::kContiguousPeakPerWidth);
    CHECK(peak_mode_from_string("across_widths") ==
          dmt::PeakFindingMode::kContiguousPeakAcrossWidths);
    CHECK(peak_mode_from_string("all") ==
          dmt::PeakFindingMode::kAllAboveThreshold);
    CHECK_THROWS_AS(peak_mode_from_string("invalid"), std::invalid_argument);
    PipelineConfig cfg;
    cfg.search.boxcar_max = 4097;
    CHECK_THROWS_AS(cfg.validate(), std::invalid_argument);
    cfg.search.boxcar_max = 4096;
    CHECK_NOTHROW(cfg.validate());
    cfg.search.max_candidates = 0;
    CHECK_THROWS_AS(cfg.validate(), std::invalid_argument);
    cfg.search.max_candidates   = 1;
    cfg.search.max_cands_per_dm = 0;
    CHECK_THROWS_AS(cfg.validate(), std::invalid_argument);
}
