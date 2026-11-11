#include <fmt/core.h>

#include <frost/pipeline/profile.hpp>

namespace frost::pipeline {

namespace {

double observed_seconds(const RunProfile& profile) {
    return static_cast<double>(profile.samples_read) *
           static_cast<double>(profile.tsamp);
}

std::string realtime_of(double seconds, double observed) {
    if (!(seconds > 0.0)) {
        return "n/a";
    }
    return fmt::format("{:.1f}x", observed / seconds);
}

void append_stage(std::string& out,
                  std::string_view name,
                  double seconds,
                  double wall,
                  double observed) {
    const double pct = wall > 0.0 ? (100.0 * seconds / wall) : 0.0;
    out += fmt::format("{:<16} {:10.4f} {:6.1f}% {:>10}\n", name, seconds, pct,
                       realtime_of(seconds, observed));
}

} // namespace

std::string format_run_profile(const RunProfile& profile) {
    const double observed = observed_seconds(profile);
    std::string out;
    out += "stage              seconds     pct   realtime\n";
    append_stage(out, "read", profile.read_s, profile.wall_s, observed);
    append_stage(out, "rfi", profile.rfi_s, profile.wall_s, observed);
    out +=
        fmt::format("  {:<14} {:10.4f}\n", "bandpass", profile.rfi_bandpass_s);
    out += fmt::format("  {:<14} {:10.4f}\n", "moments", profile.rfi_moments_s);
    out += fmt::format("  {:<14} {:10.4f}\n", "scan", profile.rfi_scan_s);
    out += fmt::format("  {:<14} {:10.4f}\n", "flags", profile.rfi_flags_s);
    out += fmt::format("  {:<14} {:10.4f}\n", "apply", profile.rfi_apply_s);
    append_stage(out, "fdmt+boxcar", profile.dedisp_s, profile.wall_s,
                 observed);
    for (const auto& step : profile.dedisp_steps) {
        out += fmt::format("  {:<14} {:10.4f}\n", step.name, step.seconds);
    }
    append_stage(out, "cluster", profile.cluster_s, profile.wall_s, observed);
    append_stage(out, "dedup", profile.dedup_s, profile.wall_s, observed);
    append_stage(out, "write", profile.write_s, profile.wall_s, observed);
    append_stage(out, "wall", profile.wall_s, profile.wall_s, observed);

    const double staged = profile.read_s + profile.rfi_s + profile.dedisp_s +
                          profile.cluster_s + profile.dedup_s + profile.write_s;
    if (profile.wall_s > 0.0 && staged > profile.wall_s * 1.05) {
        out += "read overlapped compute; stage seconds can sum above wall\n";
    }
    out += fmt::format(
        "samples read {}  searched {}  tsamp {:.6g} s  gulps {}  "
        "full-band DM trials {}  threads {}  candidates {}\n",
        profile.samples_read, profile.samples_searched, profile.tsamp,
        profile.gulps, profile.dm_trials, profile.threads, profile.candidates);
    out += fmt::format("wide boxcars (width >= {}) {}\n", kWideBoxcarSamples,
                       profile.wide_boxcars);
    out += fmt::format(
        "memory capacities (MiB): input {:.2f}, host RFI {:.2f}, engine "
        "reported {:.2f}, postprocess {:.2f}; peak pending {}\n",
        profile.input_reserved_bytes / 1048576.0,
        profile.rfi_reserved_bytes / 1048576.0,
        profile.engine_reported_bytes / 1048576.0,
        profile.postprocess_reserved_bytes / 1048576.0,
        profile.peak_pending_candidates);
    out += fmt::format("candidate drops: per-DM {}, global {}\n",
                       profile.dropped_per_dm, profile.dropped_global);
    if (profile.device_rfi_reserved_bytes ||
        profile.device_upload_reserved_bytes)
        out +=
            fmt::format("device capacities (MiB): RFI {:.2f}, upload {:.2f}\n",
                        profile.device_rfi_reserved_bytes / 1048576.0,
                        profile.device_upload_reserved_bytes / 1048576.0);
    return out;
}

} // namespace frost::pipeline
