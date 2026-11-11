#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <frost/config/config.hpp>
#include <frost/prepare/rfi.hpp>

namespace {

using frost::prepare::rfi::BlockLayout;
using frost::prepare::rfi::RFICleaner;
using frost::prepare::rfi::RfiConfig;

BlockLayout valid_layout() {
    return {.nchans     = 8,
            .max_nsamps = 256,
            .dtype      = frost::DataType::kUInt8,
            .tsamp      = 0.001,
            .f0         = 1400.0,
            .df         = -1.0};
}

void check_rejected(const RfiConfig& cfg, const char* field) {
    const auto message = Catch::Matchers::ContainsSubstring(field);
    CHECK_THROWS_WITH(cfg.validate(), message);
    frost::config::PipelineConfig pipeline;
    pipeline.rfi = cfg;
    CHECK_THROWS_WITH(pipeline.validate(), message);
    CHECK_THROWS_WITH(RFICleaner(cfg, valid_layout(), 1.0F), message);
}

} // namespace

TEST_CASE("RFI thresholds must be positive and finite even when disabled",
          "[rfi][config][robust]") {
    RfiConfig cfg;
    cfg.narrow.enabled = cfg.broad.enabled = false;
    for (const auto& [value, name] :
         {std::pair{&cfg.narrow.threshold, "narrow.threshold"},
          std::pair{&cfg.broad.threshold, "broad.threshold"},
          std::pair{&cfg.moments.threshold, "moments.threshold"},
          std::pair{&cfg.iqrm.threshold, "iqrm.threshold"},
          std::pair{&cfg.kadane_channel.threshold, "kadane_channel.threshold"},
          std::pair{&cfg.kadane_spectrum.threshold,
                    "kadane_spectrum.threshold"}}) {
        CAPTURE(name);
        const auto saved = *value;
        for (const float invalid :
             {0.0F, -1.0F, std::numeric_limits<float>::quiet_NaN(),
              std::numeric_limits<float>::infinity(),
              -std::numeric_limits<float>::infinity()}) {
            *value = invalid;
            check_rejected(cfg, name);
        }
        *value = saved;
    }
    CHECK_NOTHROW(cfg.validate());
}

TEST_CASE("RFI duration bandwidth and kurtosis limits allow zero but reject "
          "invalid values",
          "[rfi][config][robust]") {
    RfiConfig cfg;
    for (const auto& [value, name] :
         {std::pair{&cfg.moments.kurtosis_max, "moments.kurtosis_max"},
          std::pair{&cfg.kadane_channel.min_duration,
                    "kadane_channel.min_duration"},
          std::pair{&cfg.kadane_spectrum.min_bandwidth,
                    "kadane_spectrum.min_bandwidth"}}) {
        CAPTURE(name);
        const auto saved = *value;
        for (const float invalid :
             {-1.0F, std::numeric_limits<float>::quiet_NaN(),
              std::numeric_limits<float>::infinity(),
              -std::numeric_limits<float>::infinity()}) {
            *value = invalid;
            check_rejected(cfg, name);
        }
        *value = 0.0F;
        CHECK_NOTHROW(cfg.validate());
        CHECK_NOTHROW(RFICleaner(cfg, valid_layout(), 1.0F));
        *value = saved;
    }
}

TEST_CASE("RFI fractions are finite and bounded by zero and one",
          "[rfi][config][robust]") {
    RfiConfig cfg;
    for (const auto& [value, name] :
         {std::pair{&cfg.dropout.min_std_fraction, "dropout.min_std_fraction"},
          std::pair{&cfg.kadane_spectrum.whole_band_fraction,
                    "kadane_spectrum.whole_band_fraction"}}) {
        CAPTURE(name);
        const auto saved = *value;
        for (const float invalid :
             {-0.1F, 1.1F, std::numeric_limits<float>::quiet_NaN(),
              std::numeric_limits<float>::infinity(),
              -std::numeric_limits<float>::infinity()}) {
            *value = invalid;
            check_rejected(cfg, name);
        }
        for (const float boundary : {0.0F, 1.0F}) {
            *value = boundary;
            CHECK_NOTHROW(cfg.validate());
            CHECK_NOTHROW(RFICleaner(cfg, valid_layout(), 1.0F));
        }
        *value = saved;
    }
}

TEST_CASE("RFI ranges boxcars and enum settings are validated consistently",
          "[rfi][config][robust]") {
    RfiConfig cfg;
    for (const double invalid : {std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::infinity(),
                                 -std::numeric_limits<double>::infinity()}) {
        cfg.zap_freqs = {{invalid, 1500.0}};
        check_rejected(cfg, "zap_freqs");
        cfg.zap_freqs = {{1300.0, invalid}};
        check_rejected(cfg, "zap_freqs");
    }
    cfg.zap_freqs = {{1500.0, 1300.0}};
    check_rejected(cfg, "zap_freqs");
    cfg.zap_freqs = {{1400.0, 1400.0}};
    CHECK_NOTHROW(cfg.validate());
    cfg.zap_chans = {{2, 1}};
    check_rejected(cfg, "zap_chans");
    cfg                  = {};
    cfg.broad.boxcar_max = 0;
    check_rejected(cfg, "broad.boxcar_max");
    cfg      = {};
    cfg.fill = static_cast<frost::prepare::rfi::FillMode>(255);
    check_rejected(cfg, "fill");
    cfg         = {};
    cfg.zero_dm = static_cast<frost::prepare::rfi::ZeroDMMode>(255);
    check_rejected(cfg, "zero_dm");
}

TEST_CASE("RFI observing geometry rejects non-finite metadata",
          "[rfi][config][robust]") {
    for (const double invalid : {std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::infinity(),
                                 -std::numeric_limits<double>::infinity()}) {
        auto layout = valid_layout();
        layout.f0   = invalid;
        CHECK_THROWS_AS(RFICleaner({}, layout, 1.0F), std::invalid_argument);
        layout    = valid_layout();
        layout.df = invalid;
        CHECK_THROWS_AS(RFICleaner({}, layout, 1.0F), std::invalid_argument);
        layout       = valid_layout();
        layout.tsamp = invalid;
        CHECK_THROWS_AS(RFICleaner({}, layout, 1.0F), std::invalid_argument);
    }
    for (const float invalid : {std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::infinity()}) {
        CHECK_THROWS_AS(RFICleaner({}, valid_layout(), invalid),
                        std::invalid_argument);
    }
}

TEST_CASE("TOML RFI values receive the same validation as direct configuration",
          "[rfi][config][robust]") {
    for (const auto text :
         {"[rfi.moments]\nthreshold = nan\n", "[rfi.iqrm]\nthreshold = inf\n",
          "[rfi.dropout]\nmin_std_fraction = 1.1\n",
          "[rfi.kadane_channel]\nmin_duration = -0.1\n",
          "[rfi.kadane_spectrum]\nwhole_band_fraction = nan\n",
          "[rfi]\nzap_freqs = [[nan, 1500.0]]\n"}) {
        CAPTURE(text);
        CHECK_THROWS_AS(
            frost::config::PipelineConfig::from_toml_string(text).validate(),
            std::invalid_argument);
    }
}

TEST_CASE("TOML rejects non-finite numeric tokens while preserving strings and "
          "comments",
          "[rfi][config][robust]") {
    using frost::config::PipelineConfig;
    const auto cfg = PipelineConfig::from_toml_string(
        "# nan +inf -inf\n[input]\nsigproc_file = '''nan\ninf'''\n");
    CHECK(cfg.input.sigproc_file == "nan\ninf");
    for (const auto token : {"nan", "+nan", "-nan", "inf", "+inf", "-inf"}) {
        const auto text =
            std::string(
                "rfi = { fill = 'µs', dropout = { min_std_fraction = ") +
            token + " } }";
        CHECK_THROWS_WITH(
            PipelineConfig::from_toml_string(text),
            Catch::Matchers::ContainsSubstring("Non-finite TOML value"));
    }
}
