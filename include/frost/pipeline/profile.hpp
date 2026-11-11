#pragma once

#include <string>
#include <vector>

#include <frost/common/policies.hpp>

namespace frost::pipeline {

/** Candidates at least this wide (samples) are counted in the profile.
 * The benchmark uses the count as the wide-boxcar false-positive check. */
inline constexpr SizeType kWideBoxcarSamples = 1024;

/** One FDMT step, accumulated across gulps. `execute` is a full-band
 * run; `until-N` is a sub-band stop N levels before the root. */
struct DedispStepTiming {
    std::string name;
    double seconds = 0.0;
};

/**
 * Wall-clock accounting for one pipeline run. OpenMP regions are timed
 * once, not summed across threads. Stage seconds can add up to more
 * than `wall_s` when the next gulp is read while the current one is
 * searched.
 */
struct RunProfile {
    double read_s         = 0.0;
    double rfi_s          = 0.0;
    double rfi_bandpass_s = 0.0;
    double rfi_moments_s  = 0.0;
    double rfi_scan_s     = 0.0;
    double rfi_flags_s    = 0.0;
    double rfi_apply_s    = 0.0;
    double dedisp_s       = 0.0;
    std::vector<DedispStepTiming> dedisp_steps;
    double cluster_s = 0.0;
    double dedup_s   = 0.0;
    double write_s   = 0.0;
    double wall_s    = 0.0;

    // Resident capacities sampled at stage boundaries, plus planning bounds.
    SizeType input_reserved_bytes         = 0;
    SizeType rfi_reserved_bytes           = 0;
    SizeType device_rfi_reserved_bytes    = 0;
    SizeType device_upload_reserved_bytes = 0;
    SizeType engine_reported_bytes =
        0; // dmt may combine host+device allocations
    SizeType postprocess_reserved_bytes = 0;
    SizeType peak_pending_candidates    = 0; // Retained raw group members.
    SizeType dropped_global             = 0;
    SizeType dropped_per_dm             = 0;
    SizeType gulps                      = 0;
    SizeType samples_read               = 0;
    SizeType samples_searched           = 0;
    SizeType dm_trials                  = 0; // full-band trial count
    SizeType candidates                 = 0;
    SizeType wide_boxcars = 0; // written rows with width >= kWideBoxcarSamples
    int threads           = 1;
    float tsamp           = 0.0F;
};

/** Fixed-column table: seconds, percent of wall, and
 * `(samples read * tsamp) / stage time`. */
std::string format_run_profile(const RunProfile& profile);

} // namespace frost::pipeline
