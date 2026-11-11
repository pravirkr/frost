#include <cstdint>
#include <limits>
#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

#include <frost/config/config.hpp>

TEST_CASE("configuration round trips defaults and every non-default field",
          "[config]") {
    using frost::config::PipelineConfig;
    PipelineConfig cfg;
    CHECK(PipelineConfig::from_toml_string(cfg.to_toml_string()) == cfg);
    cfg.nsamps_gulp             = 8192;
    cfg.memory_limit_mib        = 4096;
    cfg.baseline_length         = 0.7F;
    cfg.verbosity               = 2;
    cfg.async_io                = false;
    cfg.profile                 = true;
    cfg.input.sigproc_file      = "a path/quoted\"file\n.fil";
    cfg.input.dada_key          = "0xdada";
    cfg.input.fswap             = true;
    cfg.input.beam              = 2;
    cfg.dedisp.dm_min           = 2.0F;
    cfg.dedisp.dm_max           = 202.0F;
    cfg.dedisp.dm_tol           = 1.1F;
    cfg.dedisp.dm_pulse_width   = 70.0F;
    cfg.dedisp.dm_dtype         = frost::DataType::kUInt16;
    cfg.dedisp.device           = dmt::Backend::kHIP;
    cfg.dedisp.device_id        = 3;
    cfg.search.boxcar_max       = 32;
    cfg.search.wtsp             = 1.5F;
    cfg.search.use_trapezoid    = true;
    cfg.search.peak_mode        = dmt::PeakFindingMode::kAllAboveThreshold;
    cfg.search.max_candidates   = 1234;
    cfg.search.max_cands_per_dm = 45;
    cfg.search.max_pending_candidates = 5678;
    cfg.search.detect_thresh          = 8.5F;
    cfg.search.cand_sep_time          = 5;
    cfg.search.cand_sep_filter        = 6;
    cfg.search.cand_sep_dm            = 7;
    cfg.search.cand_sep_dm_smear      = 9.5F;
    cfg.search.max_giant_rate         = 13.0F;
    cfg.search.cluster                = false;
    cfg.search.subband_levels         = 2;
    cfg.search.subband_dedup  = frost::search::SubbandDedupMode::kSeeded;
    cfg.output.output_dir     = "results/µs";
    cfg.output.beam           = 5;
    cfg.output.per_gulp_files = true;
    cfg.output.provenance     = true;
    auto& r                   = cfg.rfi;
    r.zap_chans               = {{1, 3}, {11, 15}};
    r.zap_freqs               = {{1400.0, 1401.25}};
    r.fill                    = frost::prepare::rfi::FillMode::kNoise;
    r.zero_dm                 = frost::prepare::rfi::ZeroDMMode::kZdot;
    r.seed                    = std::numeric_limits<uint64_t>::max();
    r.mask_grow               = 3;
    r.equalize                = true;
    r.narrow                  = {false, 3.3F};
    r.broad                   = {false, 4.4F, 8};
    r.dropout                 = {true, 0.2F};
    r.moments         = {true, 5.1F, false, false, false, false, false, 6.2F};
    r.iqrm            = {true, 2.1F, 7};
    r.kadane_channel  = {true, 9.0F, 0.13F};
    r.kadane_spectrum = {true, 8.0F, 20.0F, 0.6F};
    const auto text   = cfg.to_toml_string();
    const auto loaded = PipelineConfig::from_toml_string(text);
    CHECK(loaded == cfg);
    CHECK(loaded.to_toml_string() == text);
}

TEST_CASE("config string loading preserves strict key and type checks",
          "[config]") {
    CHECK_THROWS_AS(frost::config::PipelineConfig::from_toml_string("typo = 1"),
                    std::invalid_argument);
    CHECK_THROWS_AS(
        frost::config::PipelineConfig::from_toml_string("nsamps_gulp = -1"),
        std::invalid_argument);
}
