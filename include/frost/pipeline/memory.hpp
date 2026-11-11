#pragma once

#include <filesystem>

#include <psrio/block_source.hpp>

#include <frost/common/policies.hpp>
#include <frost/config/config.hpp>

namespace frost::pipeline {

/** Conservative component estimate for the selected memory pool. Host planning
 * covers raw input, float RFI work and
 * normalized waterfall, FDMT state/history, boxcar carry and candidates.
 * No DM-time output plane or per-thread post-FDMT search slots are allocated.
 * Component bounds include transient work; these are not allocator/RSS
 * measurements. */
struct MemoryEstimate {
    SizeType per_sample_bytes  = 0; // grows with nsamps_gulp
    SizeType fixed_bytes       = 0;
    SizeType input_bytes       = 0;
    SizeType rfi_bytes         = 0;
    SizeType fdmt_bytes        = 0;
    SizeType postprocess_bytes = 0;
    SizeType reserve_bytes     = 0;
    SizeType total_bytes       = 0; // at the configured nsamps_gulp
};

MemoryEstimate estimate_host_memory(const config::PipelineConfig& cfg,
                                    SizeType nchans,
                                    SizeType ndm,
                                    SizeType bytes_per_sample,
                                    SizeType threads,
                                    SizeType fdmt_state_bytes  = 0,
                                    SizeType fdmt_hist_bytes   = 0,
                                    SizeType search_rows       = 0,
                                    SizeType rfi_scratch_bytes = 0);

/** Largest `nsamps_gulp` whose estimate fits in `budget_bytes`; 0 when
 * even a gulp of 1 sample does not. */
SizeType max_gulp_within(const MemoryEstimate& estimate, SizeType budget_bytes);

/** Bytes this process can still allocate: `MemAvailable`, further limited
 * by the memory limit of this process's own cgroup and its ancestors (v2,
 * else v1, found through /proc/self/cgroup), less the cgroup's
 * non-reclaimable usage (page cache in `inactive_file` is not counted).
 * macOS uses free/inactive pages. 0 when availability cannot be determined. */
SizeType available_host_memory();

/** As above with the /proc and cgroup mount points given (for tests). */
SizeType available_host_memory(const std::filesystem::path& proc_root,
                               const std::filesystem::path& cgroup_root);

struct MemoryCheck {
    MemoryEstimate estimate;
    SizeType requested_bytes = 0;
    SizeType available_bytes = 0; // 0: unknown, nothing was enforced
    SizeType gulp_requested  = 0;
    SizeType gulp_used       = 0;
    bool lowered             = false;
    bool device_memory       = false;
};

/**
 * Compares the estimated host footprint of `cfg` against `available_bytes`
 * (0 queries `available_host_memory()`; if that is unknown too, nothing is
 * enforced) with 10% headroom, before execution workspaces are allocated.
 * When it does not fit: with `gulp_is_default` the gulp is lowered in
 * `cfg` to the largest multiple of 1024 samples that does (`lowered` is
 * set; the caller should warn), otherwise throws `std::runtime_error`
 * naming the estimate, the available memory and the largest gulp that fits.
 * A lowered gulp never goes below the dispersive `max_delay` at the last
 * DM trial plus `boxcar_max` (rounded up to 1024); when even that does not
 * fit, this throws.
 */
MemoryCheck check_host_memory(config::PipelineConfig& cfg,
                              const psrio::BlockSource& source,
                              bool gulp_is_default,
                              SizeType available_bytes = 0);

/** Plans large buffers before allocating them. Enforces only the selected
 * backend's pool: host RAM on CPU, device memory on GPU. memory_limit_mib=0
 * uses available memory; an explicit limit is also capped by availability.
 * Plan metadata is constructed, but no FDMT engine/workspace is allocated. */
MemoryCheck check_memory(config::PipelineConfig& cfg,
                         const psrio::BlockSource& source,
                         bool gulp_is_default,
                         SizeType available_bytes = 0);

SizeType available_device_memory(dmt::Exec exec);
} // namespace frost::pipeline
