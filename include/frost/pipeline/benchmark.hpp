#pragma once

#include <string>

#include <frost/config/config.hpp>
#include <frost/pipeline/profile.hpp>

namespace frost::pipeline {

/**
 * Result of the frozen `--benchmark` observation. `text` is the
 * workload description plus `format_run_profile`. Search, dedispersion,
 * gulp length, and RFI are overwritten to the canonical freeze;
 * verbosity, async I/O, output directory, and the FDMT device are kept.
 */
struct BenchmarkReport {
    RunProfile profile;
    std::string text;
};

BenchmarkReport run_benchmark(config::PipelineConfig cfg);

} // namespace frost::pipeline
