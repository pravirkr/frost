#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <dmt/common/backend.hpp>
#include <fmt/core.h>
#include <spdlog/spdlog.h>

#include <frost/common/version.hpp>
#include <frost/config/config.hpp>
#include <frost/dedisp/dedisp.hpp>
#include <frost/output/provenance.hpp>
#include <frost/pipeline/benchmark.hpp>
#include <frost/pipeline/input.hpp>
#include <frost/pipeline/memory.hpp>
#include <frost/pipeline/pipeline.hpp>
#include <frost/pipeline/profile.hpp>
#include <frost/prepare/rfi.hpp>

namespace {

void set_log_level(int verbosity) {
    if (verbosity <= 0) {
        spdlog::set_level(spdlog::level::warn);
    } else if (verbosity == 1) {
        spdlog::set_level(spdlog::level::info);
    } else {
        spdlog::set_level(spdlog::level::debug);
    }
}

// `frost --version`: the release first, then what a bug report needs.
std::string version_text() {
    const auto& info = frost::build_info();
    // FROST_CPU_ARCH overrides native tuning (frostCompilerOptions.cmake).
    std::string_view arch = info.native_arch == "ON" ? "native" : "portable";
    if (!info.cpu_arch.empty()) {
        arch = info.cpu_arch;
    }
    const std::string_view gpu =
        info.gpu_toolchain.empty() ? "off" : info.gpu_toolchain;
    return fmt::format("frost {}\n"
                       "revision: {}\n"
                       "dmt: {}\n"
                       "psrio: {}\n"
                       "compiler: {}\n"
                       "build: {}, GPU {}, arch {}",
                       info.version, info.revision, info.dmt_revision,
                       info.psrio_revision, info.compiler, info.build_type, gpu,
                       arch);
}

} // namespace

int main(int argc, char** argv) {
    // Pass 1: pick up --config alone (if given), so its values become
    // the defaults that pass 2's CLI flags can then override.
    CLI::App config_pass{"frost"};
    config_pass.allow_extras();
    std::string config_path;
    config_pass.add_option("--config", config_path,
                           "Path to a TOML pipeline config file");
    try {
        config_pass.parse(argc, argv);
    } catch (const CLI::ParseError&) {
        // Ignore: pass 2 below will report real parse errors properly.
    }

    frost::config::PipelineConfig cfg;
    if (!config_path.empty()) {
        try {
            cfg = frost::config::PipelineConfig::from_toml(config_path);
        } catch (const std::exception& err) {
            fmt::print(stderr, "frost: error: --config {}: {}\n", config_path,
                       err.what());
            return 1;
        }
    }

    // Pass 2: full option set, seeded from `cfg` (struct defaults, or
    // pass 1's TOML-loaded values) so that any flag the user does not
    // pass on the command line keeps that value.
    CLI::App app{"frost - FDMT-accelerated Optimal Search for Radio "
                 "Transients"};
    app.set_version_flag("--version", version_text,
                         "Print version and build information and exit");
    app.add_option("--config", config_path,
                   "Path to a TOML pipeline config file");
    auto* file_opt =
        app.add_option("--file", cfg.input.sigproc_file,
                       "Sigproc (.fil) or Breakthrough Listen HDF5 (.h5/.hdf5) "
                       "filterbank");
    app.add_option("--input-beam", cfg.input.beam,
                   "0-based beam index in a multi-beam HDF5 filterbank "
                   "(default 0). Ignored for sigproc and PSRDADA");
    auto* dada_opt =
        app.add_option("-k,--dada-key", cfg.input.dada_key,
                       "PSRDADA ring-buffer key (hex, optional 0x prefix)");
    bool benchmark = false;
    app.add_flag("--benchmark", benchmark,
                 "Run the frozen synthetic benchmark and print its profile");
    app.add_option("--memory-mib", cfg.memory_limit_mib,
                   "Memory limit in MiB for the selected backend (RAM on CPU, "
                   "device memory on GPU); 0 automatic");
    app.add_option("--max-pending-candidates",
                   cfg.search.max_pending_candidates,
                   "Cross-gulp candidate capacity; 0 uses max-candidates "
                   "(exceeding it fails)");
    app.add_flag("--provenance,!--no-provenance", cfg.output.provenance,
                 "Write effective configuration and run metadata to run.toml");
    app.add_flag("--profile,!--no-profile", cfg.profile,
                 "Print the accumulated stage profile at the end of the run");
    app.add_flag("--async-io,!--no-async-io", cfg.async_io,
                 "Read the next gulp while the current one is searched");
    app.add_flag("--fswap,!--no-fswap", cfg.input.fswap,
                 "Reverse channel order for negative-DM searching");
    app.add_option("--output-dir", cfg.output.output_dir,
                   "Directory for the .cand output file(s)");
    app.add_option("--beam", cfg.output.beam,
                   "Override the 1-based beam number used in output naming");
    app.add_flag("--per-gulp-files,!--single-file", cfg.output.per_gulp_files,
                 "Write <utc>_<beam>.cand files, one per gulp");
    auto* gulp_opt = app.add_option("--nsamps-gulp", cfg.nsamps_gulp,
                                    "Number of samples to process per gulp");
    app.add_option("--baseline-length", cfg.baseline_length,
                   "Baseline-smoothing window length (seconds)");
    // CLI11 resets a bound integer flag to 0 when it is absent, which would
    // overwrite a TOML `verbosity`; count into a local and apply it only
    // when -v was given.
    int verbose_count = 0;
    app.add_flag("-v,--verbose", verbose_count,
                 "Increase verbosity (-v per-gulp info, -vv stage timings)");

    auto& rfi = cfg.rfi;
    app.add_flag("--clean-broad,!--no-clean-broad", rfi.broad.enabled,
                 "Broad-band (zero-DM time-sample) RFI excision");
    app.add_flag("--clean-narrow,!--no-clean-narrow", rfi.narrow.enabled,
                 "Narrow-band (per-cell) RFI excision");
    float rfi_tol     = rfi.narrow.threshold;
    auto* rfi_tol_opt = app.add_option(
        "--rfi-tol", rfi_tol, "Narrow- and broad-band RFI threshold (sigma)");
    app.add_option("--rfi-broad-boxcar-max", rfi.broad.boxcar_max,
                   "Widest zero-DM boxcar the broad-band cleaner zaps");
    std::vector<std::pair<frost::SizeType, frost::SizeType>> zap_chans;
    app.add_option("--zap-chans", zap_chans,
                   "Zap channels START..END (inclusive); repeatable")
        ->allow_extra_args(false);
    std::string kill_file;
    app.add_option("--kill-file", kill_file,
                   "Channel kill file: one 1 (keep) or 0 (kill) per line, one "
                   "line per channel in the file's stored order (# comments "
                   "allowed); must list exactly nchans channels")
        ->check(CLI::ExistingFile);
    std::vector<std::pair<double, double>> zap_freqs;
    app.add_option("--zap-freqs", zap_freqs,
                   "Zap channels within LO..HI MHz (inclusive); repeatable")
        ->allow_extra_args(false);
    std::string fill_str = frost::config::to_string(rfi.fill);
    app.add_option("--rfi-fill", fill_str,
                   "Replacement for flagged samples (neighbour|mean|noise)")
        ->check(CLI::IsMember({"neighbour", "mean", "noise"}));
    app.add_option("--rfi-seed", rfi.seed, "Seed for random RFI replacement");
    std::string zero_dm_str = frost::config::to_string(rfi.zero_dm);
    app.add_option("--rfi-zero-dm", zero_dm_str,
                   "Zero-DM filter (off|subtract|zdot)")
        ->check(CLI::IsMember({"off", "subtract", "zdot"}));
    app.add_flag("--rfi-equalize,!--no-rfi-equalize", rfi.equalize,
                 "Rewrite every channel to a common level and rms");
    app.add_flag("--rfi-dropout,!--no-rfi-dropout", rfi.dropout.enabled,
                 "Flag dropped/blanked time samples");
    app.add_flag("--rfi-moments,!--no-rfi-moments", rfi.moments.enabled,
                 "Mask channels with outlying mean/std/skewness/kurtosis/"
                 "autocorrelation");
    app.add_flag("--rfi-iqrm,!--no-rfi-iqrm", rfi.iqrm.enabled,
                 "Mask channels flagged by IQRM on their standard deviation");
    app.add_option("--rfi-mask-grow", rfi.mask_grow,
                   "Widen moments/IQRM channel masks by N channels each side");
    app.add_flag("--rfi-kadane-channel,!--no-rfi-kadane-channel",
                 rfi.kadane_channel.enabled,
                 "Flag long-lived runs along time in each channel (Kadane)");
    app.add_flag("--rfi-kadane-spectrum,!--no-rfi-kadane-spectrum",
                 rfi.kadane_spectrum.enabled,
                 "Flag wide runs across frequency in each spectrum (Kadane)");

    std::string device_name{dmt::to_string(cfg.dedisp.device)};
    app.add_option("--device,--dedisp-backend", device_name,
                   "Execution backend (cpu|cuda|hip). --dedisp-backend "
                   "is an alias of --device");
    app.add_option("--device-id", cfg.dedisp.device_id,
                   "GPU ordinal passed to cuda or hip (default 0)");
    app.add_option("--dm-min", cfg.dedisp.dm_min, "Minimum DM to search");
    app.add_option("--dm-max", cfg.dedisp.dm_max, "Maximum DM to search");
    app.add_option("--dm-tol", cfg.dedisp.dm_tol,
                   "Smear tolerance factor between DM trials");
    app.add_option("--dm-pulse-width", cfg.dedisp.dm_pulse_width,
                   "Expected intrinsic pulse width (microseconds)");
    int dm_nbits = frost::is_floating(cfg.dedisp.dm_dtype)
                       ? 32
                       : frost::bits_of(cfg.dedisp.dm_dtype);
    app.add_option(
           "--dm-nbits", dm_nbits,
           "Bits per dedispersed sample; FDMT writes float32, so only 32 "
           "is accepted")
        ->check(CLI::IsMember({8, 16, 32}));

    app.add_option("--boxcar-max", cfg.search.boxcar_max,
                   "Maximum boxcar width to convolve with");
    app.add_option("--wtsp", cfg.search.wtsp,
                   "Boxcar width spacing (>=1). 2 doubles, 1 is every "
                   "integer, 1.5 is geometric");
    app.add_flag("--trapezoid,!--no-trapezoid", cfg.search.use_trapezoid,
                 "Search widths >= 3 with trapezoids (half-weight edges) "
                 "instead of boxcars");
    std::string peak_mode = frost::config::to_string(cfg.search.peak_mode);
    app.add_option("--peak-mode", peak_mode,
                   "Peak reduction (per_width|across_widths|all)")
        ->check(CLI::IsMember({"per_width", "across_widths", "all"}));
    app.add_option("--max-candidates", cfg.search.max_candidates,
                   "Candidate capacity per engine call");
    app.add_option("--max-cands-per-dm", cfg.search.max_cands_per_dm,
                   "Candidate capacity per plane and DM");
    app.add_option("--detect-thresh", cfg.search.detect_thresh,
                   "Detection threshold (std. dev.)");
    app.add_option("--cand-sep-time", cfg.search.cand_sep_time,
                   "Min candidate separation in time (samples)");
    app.add_option("--cand-sep-filter", cfg.search.cand_sep_filter,
                   "Min candidate separation in filter index");
    app.add_option("--cand-sep-dm", cfg.search.cand_sep_dm,
                   "Max candidate separation in DM trials");
    app.add_option("--cand-sep-dm-smear", cfg.search.cand_sep_dm_smear,
                   "Beyond adjacent trials, cluster only within this many "
                   "boxcar widths of dispersive smearing in DM "
                   "(0 = trial count only)")
        ->check(CLI::NonNegativeNumber);
    app.add_option("--max-giant-rate", cfg.search.max_giant_rate,
                   "Max allowed giants per minute (<=0 disables)");
    std::string subband_dedup_str =
        cfg.search.subband_dedup == frost::search::SubbandDedupMode::kSeeded
            ? "seeded"
            : "transitive";
    app.add_option("--subband-dedup", subband_dedup_str,
                   "How sub-band dedup groups matches: transitive (a nested "
                   "row may join two wide rows, absorbing DM wings) or "
                   "seeded (each row joins one group; no bridging)")
        ->check(CLI::IsMember({"transitive", "seeded"}));
    app.add_flag("--cluster,!--no-cluster", cfg.search.cluster,
                 "Cluster before writing: friends-of-friends within each "
                 "FDMT plane, then deduplicate nested sub-bands on the "
                 "DM curve. --no-cluster writes every detection");
    int subband_levels = static_cast<int>(cfg.search.subband_levels);
    app.add_option("--subband-levels", subband_levels,
                   "FDMT sub-band search depth: 0 full band only, "
                   "1 searches full band and 3 half bands, "
                   "2 also searches 7 quarter bands (including overlaps)")
        ->check(CLI::Range(0, 2));

    CLI11_PARSE(app, argc, argv);
    // An input given on the command line replaces the other kind of input
    // a --config file may have set; naming both on the command line stays
    // an error below.
    if (file_opt->count() > 0 && dada_opt->count() == 0) {
        cfg.input.dada_key.clear();
    } else if (dada_opt->count() > 0 && file_opt->count() == 0) {
        cfg.input.sigproc_file.clear();
    }
    if (verbose_count > 0) {
        cfg.verbosity = verbose_count;
    }
    cfg.search.subband_levels = static_cast<frost::SizeType>(subband_levels);
    cfg.search.peak_mode      = frost::config::peak_mode_from_string(peak_mode);
    cfg.search.subband_dedup =
        subband_dedup_str == "seeded"
            ? frost::search::SubbandDedupMode::kSeeded
            : frost::search::SubbandDedupMode::kTransitive;
    try {
        cfg.dedisp.device = dmt::parse_backend(device_name);
    } catch (const std::invalid_argument& err) {
        fmt::print(stderr, "frost: error: {}\n", err.what());
        return 1;
    }
    if (cfg.dedisp.device_id < 0) {
        fmt::print(stderr, "frost: error: --device-id must be >= 0\n");
        return 1;
    }
    cfg.dedisp.dm_dtype = frost::config::dm_dtype_from_nbits(dm_nbits);
    if (dm_nbits != 32) {
        fmt::print(stderr, "frost: error: FDMT writes float32 DM-time samples; "
                           "--dm-nbits must be 32\n");
        return 1;
    }
    for (const auto& [start, end] : zap_chans) {
        if (end < start) {
            fmt::print(stderr, "frost: error: --zap-chans {} {}: END < START\n",
                       start, end);
            return 1;
        }
        cfg.rfi.zap_chans.push_back({start, end});
    }
    for (const auto& [lo, hi] : zap_freqs) {
        if (hi < lo) {
            fmt::print(stderr, "frost: error: --zap-freqs {} {}: HI < LO\n", lo,
                       hi);
            return 1;
        }
        cfg.rfi.zap_freqs.push_back({lo, hi});
    }
    if (rfi_tol_opt->count() > 0) {
        rfi.narrow.threshold = rfi_tol;
        rfi.broad.threshold  = rfi_tol;
    }
    rfi.fill    = frost::config::fill_mode_from_string(fill_str);
    rfi.zero_dm = frost::config::zero_dm_from_string(zero_dm_str);

    const bool have_file = !cfg.input.sigproc_file.empty();
    const bool have_dada = !cfg.input.dada_key.empty();
    const int n_sources =
        (have_file ? 1 : 0) + (have_dada ? 1 : 0) + (benchmark ? 1 : 0);
    if (n_sources != 1) {
        fmt::print(stderr, "frost: error: pass exactly one of --file, "
                           "--dada-key, or --benchmark\n");
        return 1;
    }

    std::optional<frost::output::RunProvenancePlan> provenance;
    std::vector<std::string> command_line;
    for (int i = 0; i < argc; ++i)
        command_line.emplace_back(argv[i]);
    try {
        set_log_level(cfg.verbosity);
        if (benchmark) {
            fmt::print(stderr, "frost: running the frozen benchmark\n");
            const auto report = frost::pipeline::run_benchmark(cfg);
            fmt::print("{}", report.text);
            return 0;
        }
        cfg.validate();

        auto source = frost::pipeline::open_input(cfg.input);
        if (!kill_file.empty()) {
            // Stored-order channels; --fswap reverses the delivered order
            // that zap_chans index. Merged into zap_chans, so provenance
            // records the effective channels.
            const auto nchans = static_cast<frost::SizeType>(source.nchans());
            const auto killed =
                frost::prepare::rfi::read_kill_file(kill_file, nchans);
            frost::SizeType count = 0;
            for (const auto& range : killed) {
                count += range.end - range.start + 1;
                cfg.rfi.zap_chans.push_back(
                    cfg.input.fswap
                        ? frost::prepare::rfi::ChannelRange{nchans - 1 -
                                                                range.end,
                                                            nchans - 1 -
                                                                range.start}
                        : range);
            }
            spdlog::info("Kill file {}: {} of {} channels killed", kill_file,
                         count, nchans);
        }
        frost::pipeline::MemoryCheck memory;
        {
            // The default gulp (262144 samples) needs tens of GB at
            // wideband channel counts: fit it to the machine, or refuse
            // clearly, instead of being OOM-killed with no message.
            const bool gulp_is_default =
                gulp_opt->count() == 0 &&
                cfg.nsamps_gulp == frost::config::PipelineConfig{}.nsamps_gulp;
            memory =
                frost::pipeline::check_memory(cfg, source, gulp_is_default);
            if (memory.lowered) {
                spdlog::warn(
                    "Default nsamps_gulp {} needs about {:.1f} GiB; "
                    "using {} to fit {:.1f} GiB of available memory "
                    "(set --nsamps-gulp to choose)",
                    memory.gulp_requested,
                    static_cast<double>(memory.requested_bytes) / 1073741824.0,
                    memory.gulp_used,
                    static_cast<double>(memory.available_bytes) / 1073741824.0);
            }
            spdlog::info(
                "Memory components (MiB): input {:.1f}, RFI {:.1f}, "
                "FDMT/search {:.1f}, postprocess {:.1f}, reserve {:.1f}",
                memory.estimate.input_bytes / 1048576.0,
                memory.estimate.rfi_bytes / 1048576.0,
                memory.estimate.fdmt_bytes / 1048576.0,
                memory.estimate.postprocess_bytes / 1048576.0,
                memory.estimate.reserve_bytes / 1048576.0);
            spdlog::info("Estimated {} memory {:.2f} GiB (nsamps_gulp {})",
                         memory.device_memory ? "device" : "host",
                         static_cast<double>(memory.estimate.total_bytes) /
                             1073741824.0,
                         cfg.nsamps_gulp);
        }
        // A component budget is computed without constructing an FDMT engine.
        const std::string label =
            have_file ? cfg.input.sigproc_file : cfg.input.dada_key;
        if (cfg.output.provenance) {
            provenance.emplace(std::filesystem::path(cfg.output.output_dir) /
                                   "run.toml",
                               cfg, source, command_line, &memory);
            provenance->execute("started", {});
        }
        frost::pipeline::Pipeline pipeline(cfg, source);

        frost::SizeType total_read = 0;
        frost::SizeType gulp_count = 0;
        while (const auto nsamps_read = pipeline.execute()) {
            total_read += nsamps_read;
            ++gulp_count;
        }
        if (total_read == 0) {
            // A header with no complete sample row (truncated file) or a
            // ring that closed at once is a failed run, not an empty search.
            throw std::runtime_error("input contained no complete samples: " +
                                     label);
        }
        fmt::print("frost: read {} sample(s) in {} gulp(s) and searched {} "
                   "from {}\n",
                   total_read, gulp_count, pipeline.nsamps_processed(), label);
        if (provenance)
            provenance->execute("complete", pipeline.profile());
        if (cfg.profile) {
            fmt::print("{}",
                       frost::pipeline::format_run_profile(pipeline.profile()));
        }
    } catch (const std::exception& e) {
        if (provenance) {
            try {
                provenance->execute("failed", {}, e.what());
            } catch (const std::exception& metadata_error) {
                spdlog::warn("Failed to update run provenance: {}",
                             metadata_error.what());
            }
        }
        fmt::print(stderr, "frost: error: {}\n", e.what());
        return 1;
    }
    return 0;
}
