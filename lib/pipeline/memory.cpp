#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <omp.h>

#include <frost/pipeline/memory.hpp>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

#include <dmt/common/boxcar_plans.hpp>
#include <dmt/common/candidate.hpp>
#include <dmt/dmt.hpp>

#include <frost/dedisp/dedisp.hpp>
#include <frost/pipeline/pipeline.hpp>
#include <frost/search/candidate.hpp>
#include <frost/search/postprocess.hpp>

#include "lib/prepare/device_rfi.hpp"

namespace frost::pipeline {

namespace {

constexpr SizeType kMiB = SizeType{1} << 20U;
constexpr SizeType kGiB = SizeType{1} << 30U;

// Runtime, plan tables and bounded RFI statistics, with 10% headroom below.
constexpr SizeType kFixedBase    = 64 * kMiB;
constexpr SizeType kFixedPerChan = 40 * 1024;
constexpr SizeType kGulpRounding = 1024;

SizeType saturating_mul(SizeType a, SizeType b) {
    if (a != 0 && b > std::numeric_limits<SizeType>::max() / a) {
        return std::numeric_limits<SizeType>::max();
    }
    return a * b;
}

SizeType saturating_add(SizeType a, SizeType b) {
    return b > std::numeric_limits<SizeType>::max() - a
               ? std::numeric_limits<SizeType>::max()
               : a + b;
}

namespace fs = std::filesystem;

constexpr SizeType kNoLimit = std::numeric_limits<SizeType>::max();
// cgroup v1 reports a huge page-aligned number rather than "max".
constexpr SizeType kV1Unlimited = SizeType{1} << 60U;

// Reads the first unsigned integer of a file, or `fallback`.
SizeType read_number(const fs::path& path, SizeType fallback) {
    std::ifstream in(path);
    std::string token;
    if (!(in >> token)) {
        return fallback;
    }
    try {
        return static_cast<SizeType>(std::stoull(token));
    } catch (const std::exception&) {
        return fallback; // "max"
    }
}

// Value of `key` in a "key value" file (memory.stat), or 0.
SizeType read_stat(const fs::path& path, std::string_view key) {
    std::ifstream in(path);
    std::string name;
    unsigned long long value = 0;
    while (in >> name >> value) {
        if (name == key) {
            return static_cast<SizeType>(value);
        }
    }
    return 0;
}

SizeType meminfo_available(const fs::path& proc_root) {
    std::ifstream in(proc_root / "meminfo");
    std::string key;
    while (in >> key) {
        unsigned long long kb = 0;
        if (key == "MemAvailable:" && (in >> kb)) {
            return static_cast<SizeType>(kb) * 1024;
        }
        in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    return 0;
}

// This process's cgroup path for the v2 unified hierarchy (`v2`) or the
// v1 memory controller, from /proc/self/cgroup ("id:controllers:path").
std::string own_cgroup(const fs::path& proc_root, bool v2) {
    std::ifstream in(proc_root / "self" / "cgroup");
    std::string line;
    while (std::getline(in, line)) {
        const auto c1 = line.find(':');
        const auto c2 = c1 == std::string::npos ? c1 : line.find(':', c1 + 1);
        if (c2 == std::string::npos) {
            continue;
        }
        const std::string id   = line.substr(0, c1);
        const std::string ctrl = line.substr(c1 + 1, c2 - c1 - 1);
        const std::string path = line.substr(c2 + 1);
        if (v2 && id == "0" && ctrl.empty()) {
            return path;
        }
        if (!v2) {
            std::istringstream names(ctrl);
            std::string name;
            while (std::getline(names, name, ',')) {
                if (name == "memory") {
                    return path;
                }
            }
        }
    }
    return {};
}

// Smallest room (limit minus non-reclaimable usage) over the cgroup at
// `rel` under `base` and each of its ancestors up to `base`. Page cache
// that the kernel can drop (`inactive_file`) does not count as used.
// kNoLimit when no level has a limit.
SizeType cgroup_room(const fs::path& base, const std::string& rel, bool v2) {
    if (!fs::exists(base)) {
        return kNoLimit;
    }
    fs::path dir = base / fs::path(rel).relative_path();
    if (!fs::exists(dir)) {
        dir = base; // a namespaced path that is not visible here
    }
    const char* limit_file = v2 ? "memory.max" : "memory.limit_in_bytes";
    const char* usage_file = v2 ? "memory.current" : "memory.usage_in_bytes";
    const char* cache_key  = v2 ? "inactive_file" : "total_inactive_file";
    const fs::path stop    = base.lexically_normal();
    SizeType room          = kNoLimit;
    while (true) {
        const SizeType limit = read_number(dir / limit_file, kNoLimit);
        if (limit != kNoLimit && limit < kV1Unlimited) {
            const SizeType used  = read_number(dir / usage_file, 0);
            const SizeType cache = read_stat(dir / "memory.stat", cache_key);
            const SizeType held  = used > cache ? used - cache : 0;
            room = std::min(room, limit > held ? limit - held : 0);
        }
        if (dir.lexically_normal() == stop || !dir.has_parent_path() ||
            dir.parent_path() == dir) {
            break;
        }
        dir = dir.parent_path();
    }
    return room;
}

// Upper bound of the dispersive delay (samples) FDMT will need across the
// band at the last DM trial, with a small margin. The Pipeline constructor
// checks the exact value from the engine.
std::string gib(SizeType bytes) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f GiB",
                  static_cast<double>(bytes) / static_cast<double>(kGiB));
    return buf;
}

} // namespace

MemoryEstimate estimate_host_memory(const config::PipelineConfig& cfg,
                                    SizeType nchans,
                                    SizeType ndm,
                                    SizeType bytes_per_sample,
                                    SizeType threads,
                                    SizeType fdmt_state_bytes,
                                    SizeType fdmt_hist_bytes,
                                    SizeType search_rows,
                                    SizeType rfi_scratch_bytes) {
    MemoryEstimate est;
    // GulpPrefetch uses two buffers with prefetch, one synchronously. The
    // cleaner owns float work and normalized output; the engine never
    // materializes DMT.
    const SizeType per_sample = saturating_add(
        128,
        saturating_add(saturating_mul(cfg.async_io ? 2 : 1, bytes_per_sample),
                       saturating_mul(2 * sizeof(float), nchans)));
    // Without exact plan rows, bound each searched plane by the root DM
    // count. At depth k, dmt searches 2^k tree and 2^k - 1 staggered bands.
    SizeType plane_count = 1;
    SizeType tree_bands  = 1;
    for (SizeType depth = 0; depth < cfg.search.subband_levels; ++depth) {
        tree_bands = saturating_mul(tree_bands, 2);
        plane_count =
            saturating_add(plane_count, saturating_mul(tree_bands, 2) - 1);
    }
    const SizeType rows =
        search_rows ? search_rows : saturating_mul(ndm, plane_count);
    const auto widths    = dmt::plans::generate_box_width_trials(
                               cfg.search.boxcar_max, cfg.search.wtsp)
                               .size();
    const SizeType carry = saturating_mul(
        rows, saturating_mul(cfg.search.boxcar_max, sizeof(float)));
    // Pending excursions, threshold/sigma tables and per-row counters.
    const SizeType peaks =
        saturating_mul(rows, saturating_add(saturating_mul(widths, 96), 64));
    const SizeType candidates =
        saturating_mul(cfg.search.max_candidates, sizeof(dmt::RawCandidate));
    const SizeType scratch =
        saturating_mul(threads, saturating_mul(cfg.search.boxcar_max + 4096,
                                               2 * sizeof(double)));
    est.input_bytes =
        saturating_mul(saturating_mul(cfg.async_io ? 2 : 1, bytes_per_sample),
                       cfg.nsamps_gulp);
    const SizeType float_and_rows = saturating_mul(
        saturating_add(saturating_mul(2 * sizeof(float), nchans), 128),
        cfg.nsamps_gulp);
    const SizeType rfi_extra = rfi_scratch_bytes
                                   ? (rfi_scratch_bytes > float_and_rows
                                          ? rfi_scratch_bytes - float_and_rows
                                          : 0)
                                   : saturating_mul(kFixedPerChan, nchans);
    est.rfi_bytes            = saturating_add(float_and_rows, rfi_extra);
    est.fdmt_bytes           = 0;
    for (const auto bytes :
         {fdmt_state_bytes, fdmt_hist_bytes, carry, peaks, candidates, scratch})
        est.fdmt_bytes = saturating_add(est.fdmt_bytes, bytes);
    est.postprocess_bytes =
        search::CandidatePostprocessPlan::estimate_memory(cfg.search);
    SizeType fixed       = saturating_add(kFixedBase, rfi_extra);
    fixed                = saturating_add(fixed, est.fdmt_bytes);
    fixed                = saturating_add(fixed, est.postprocess_bytes);
    est.per_sample_bytes = saturating_add(per_sample, per_sample / 10);
    est.fixed_bytes      = saturating_add(fixed, fixed / 10);
    est.total_bytes      = saturating_add(
        saturating_mul(est.per_sample_bytes, cfg.nsamps_gulp), est.fixed_bytes);
    const auto components =
        saturating_add(saturating_add(est.input_bytes, est.rfi_bytes),
                       saturating_add(est.fdmt_bytes, est.postprocess_bytes));
    est.reserve_bytes =
        est.total_bytes >= components ? est.total_bytes - components : 0;
    return est;
}

SizeType max_gulp_within(const MemoryEstimate& estimate,
                         SizeType budget_bytes) {
    if (budget_bytes <= estimate.fixed_bytes ||
        estimate.per_sample_bytes == 0) {
        return 0;
    }
    return (budget_bytes - estimate.fixed_bytes) / estimate.per_sample_bytes;
}

SizeType available_host_memory(const std::filesystem::path& proc_root,
                               const std::filesystem::path& cgroup_root) {
    SizeType avail = meminfo_available(proc_root);
    // cgroup v2 (unified), then the v1 memory controller.
    SizeType room = cgroup_room(cgroup_root, own_cgroup(proc_root, true), true);
    if (room == kNoLimit) {
        room = cgroup_room(cgroup_root / "memory", own_cgroup(proc_root, false),
                           false);
    }
    if (room != kNoLimit) {
        avail = avail == 0 ? room : std::min(avail, room);
    }
    return avail;
}

SizeType available_host_memory() {
#ifdef __APPLE__
    const auto host = mach_host_self();
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page_size          = 0;
    const auto page_status       = host_page_size(host, &page_size);
    const auto vm_status         = host_statistics64(
        host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count);
    mach_port_deallocate(mach_task_self(), host);
    if (page_status != KERN_SUCCESS || vm_status != KERN_SUCCESS)
        return 0;
    return saturating_mul(saturating_add(vm.free_count, vm.inactive_count),
                          page_size);
#else
    return available_host_memory("/proc", "/sys/fs/cgroup");
#endif
}

MemoryCheck check_memory(config::PipelineConfig& cfg,
                         const psrio::BlockSource& source,
                         bool gulp_is_default,
                         SizeType available_bytes) {
    MemoryCheck out;
    cfg.validate();
    out.device_memory  = cfg.dedisp.device != dmt::Backend::kCPU;
    out.gulp_requested = cfg.nsamps_gulp;
    out.gulp_used      = cfg.nsamps_gulp;
    out.available_bytes =
        available_bytes != 0 ? available_bytes
        : out.device_memory
            ? available_device_memory({.backend = cfg.dedisp.device,
                                       .device  = cfg.dedisp.device_id})
            : available_host_memory();
    if (cfg.memory_limit_mib) {
        const auto limit = cfg.memory_limit_mib * kMiB;
        out.available_bytes =
            out.available_bytes ? std::min(out.available_bytes, limit) : limit;
    }

    const auto plan = make_dedisp_plan_config(cfg, source);
    const auto threads =
        static_cast<SizeType>(std::max(1, omp_get_max_threads()));
    // Plan construction allocates tables only. Bound both unfused tree state
    // halves using dmt's automatic chunk budget (2 GiB, minimum 2048).
    const auto fdmt     = dedisp::make_dmt_fdmt_plan(plan);
    const SizeType n    = fdmt.get_dm_grid_final().size();
    SizeType state_rows = 0, search_rows = 0;
    const auto& shapes = fdmt.get_container().state_shape;
    for (SizeType level = 0; level < fdmt.get_niters(); ++level) {
        state_rows = std::max(state_rows, shapes[level].ncoords);
    }
    for (SizeType remaining = 0; remaining <= cfg.search.subband_levels;
         ++remaining) {
        if (remaining > fdmt.get_niters())
            break;
        search_rows = saturating_add(
            search_rows, shapes[fdmt.get_niters() - remaining].ncoords);
    }
    // CPU fusion uses two scratch halves per worker. Bound the deepest
    // possible fused tree group, without selecting/constructing an engine.
    // Auto fusion requires two groups per worker above level one; its valid-
    // mode time tiles are at most 512 samples in the pinned dmt implementation.
    const auto& container = fdmt.get_container();
    SizeType fuse = std::min({fdmt.get_max_fuse_levels(), fdmt.get_niters() - 1,
                              fdmt.get_niters() - cfg.search.subband_levels});
    while (fuse > 1 && shapes[fuse].nchans < saturating_mul(2, threads))
        --fuse;
    SizeType fused_rows = 0;
    for (SizeType level = 0; level < fuse; ++level) {
        SizeType node = 0;
        for (SizeType group = 0; group < shapes[fuse].nchans; ++group) {
            const auto& extent = container.grids[fuse][group];
            SizeType rows      = 0;
            while (node < shapes[level].nchans &&
                   container.grids[level][node].chan_begin < extent.chan_end) {
                rows = saturating_add(rows, container.grids[level][node].ndt);
                ++node;
            }
            fused_rows = std::max(fused_rows, rows);
        }
    }
    SizeType op_count = 0;
    for (const auto& level : container.coordinates)
        op_count = saturating_add(op_count, level.size());
    // Ten SizeType fields per CPU merge op plus ring positions/alignment;
    // box-weight/rounding tables are bounded by the full-band delay support.
    const SizeType extra_plan = saturating_add(
        saturating_mul(op_count, 12 * sizeof(SizeType)),
        saturating_mul(static_cast<SizeType>(fdmt.get_dt_max()) + 1, 64));
    const SizeType state_per_sample =
        saturating_mul(2 * sizeof(float), state_rows);
    const SizeType history = saturating_add(
        saturating_mul(sizeof(float),
                       saturating_add(fdmt.get_history_size(),
                                      fdmt.get_tree_history_size())),
        saturating_add(
            saturating_mul(3, fdmt.get_container().get_memory_usage()),
            extra_plan));
    const auto estimate = [&](SizeType gulp) {
        auto trial        = cfg;
        trial.nsamps_gulp = gulp;
        SizeType chunk    = 2048;
        while (chunk < gulp &&
               saturating_mul(state_per_sample, 2 * chunk) <= 2 * kGiB)
            chunk *= 2;
        const SizeType state =
            saturating_mul(state_per_sample, std::min(chunk, gulp));
        auto layout    = rfi_block_layout(source, gulp);
        const auto rfi = prepare::rfi::RFICleaner::estimate_memory(
            cfg.rfi, layout, cfg.baseline_length, threads);
        const auto active_chunk = std::min(chunk, gulp);
        const auto row_bytes = ((active_chunk * sizeof(float) + 63) / 64) * 64;
        const auto fused_bytes = saturating_add(
            saturating_mul(saturating_mul(fused_rows, std::min<SizeType>(
                                                          512, active_chunk)),
                           sizeof(float)),
            64);
        const auto cpu_workspace =
            out.device_memory
                ? 0
                : saturating_mul(
                      threads, saturating_add(saturating_mul(3, row_bytes),
                                              saturating_mul(2, fused_bytes)));
        auto est = estimate_host_memory(
            trial, static_cast<SizeType>(source.nchans()), n,
            static_cast<SizeType>(source.bytes_per_sample()), threads,
            saturating_add(state, cpu_workspace), history, search_rows, rfi);
        if (!out.device_memory)
            return est;
        // Device FDMT workspace is conservatively bounded by the unfused tree,
        // history and analytical-search terms. CPU-only postprocessing is not
        // charged to the GPU. Device RFI has three sampled-bandpass work
        // arrays.
        const bool device_rfi = device::DeviceRfi::default_integer_neighbour(
            cfg.rfi, to_data_type(source.sample_type()));
        const auto nchans         = static_cast<SizeType>(source.nchans());
        const SizeType per_sample = saturating_add(
            saturating_mul(sizeof(float), nchans),
            device_rfi ? saturating_add(
                             saturating_mul(2, source.bytes_per_sample()), 32)
                       : 0);
        const auto window = std::max<SizeType>(
            5, static_cast<SizeType>(std::min(
                   static_cast<double>(gulp),
                   static_cast<double>(cfg.baseline_length) / source.tsamp())));
        const auto windows = std::max<SizeType>(1, gulp / window);
        const SizeType rfi_tables =
            device_rfi
                ? saturating_add(
                      saturating_mul(nchans, 3 * 3125 * sizeof(float) + 64),
                      saturating_mul(saturating_mul(windows, nchans),
                                     3 * sizeof(float)))
                : 0;
        est.input_bytes =
            saturating_mul(saturating_mul(sizeof(float), nchans), gulp);
        est.rfi_bytes =
            device_rfi
                ? saturating_add(
                      rfi_tables,
                      saturating_mul(
                          saturating_add(
                              saturating_mul(2, source.bytes_per_sample()), 32),
                          gulp))
                : 0;
        est.postprocess_bytes = 0;
        const auto fixed      = saturating_add(
            kFixedBase, saturating_add(est.fdmt_bytes, rfi_tables));
        est.per_sample_bytes = saturating_add(per_sample, per_sample / 10);
        est.fixed_bytes      = saturating_add(fixed, fixed / 10);
        est.total_bytes      = saturating_add(
            saturating_mul(est.per_sample_bytes, gulp), est.fixed_bytes);
        const auto components = saturating_add(
            saturating_add(est.input_bytes, est.rfi_bytes), est.fdmt_bytes);
        est.reserve_bytes =
            est.total_bytes >= components ? est.total_bytes - components : 0;
        return est;
    };
    out.estimate        = estimate(cfg.nsamps_gulp);
    out.requested_bytes = out.estimate.total_bytes;
    if (out.available_bytes == 0) {
        return out; // cannot tell; do not guess
    }
    const SizeType budget = out.available_bytes - (out.available_bytes / 10);
    if (out.estimate.total_bytes <= budget) {
        return out;
    }
    // Tree state changes at automatic chunk boundaries, so invert the full
    // estimate rather than keeping the requested gulp's state allocation.
    SizeType lo = 0, hi = cfg.nsamps_gulp;
    while (lo < hi) {
        const SizeType mid = lo + (hi - lo + 1) / 2;
        if (estimate(mid).total_bytes <= budget)
            lo = mid;
        else
            hi = mid - 1;
    }
    const SizeType fits = lo;
    const std::string what =
        std::string("estimated ") + (out.device_memory ? "device" : "host") +
        " memory " + gib(out.estimate.total_bytes) + " for " +
        std::to_string(source.nchans()) + " channels, " + std::to_string(n) +
        " DM trials and nsamps_gulp " + std::to_string(cfg.nsamps_gulp) +
        " exceeds the " + gib(budget) + " available (90% of " +
        gib(out.available_bytes) + ")";
    const SizeType rounded = (fits / kGulpRounding) * kGulpRounding;
    // The gulp must exceed max_delay + boxcar_max (the Pipeline constructor
    // refuses anything smaller), so never lower it below that.
    const SizeType delay = static_cast<SizeType>(fdmt.get_dt_max());
    const SizeType needed =
        saturating_add(saturating_add(delay, cfg.search.boxcar_max), 1);
    const SizeType floor_gulp = std::max<SizeType>(
        saturating_mul((needed + kGulpRounding - 1) / kGulpRounding,
                       kGulpRounding),
        kGulpRounding);
    if (rounded < floor_gulp) {
        throw std::runtime_error(
            what + "; not even nsamps_gulp " + std::to_string(floor_gulp) +
            " (about max_delay " + std::to_string(delay) + " + boxcar_max " +
            std::to_string(cfg.search.boxcar_max) +
            ") fits. Reduce dm_max, use fewer channels, or use a larger "
            "machine (or raise the selected-backend memory limit)");
    }
    if (!gulp_is_default) {
        throw std::runtime_error(
            what + "; the largest nsamps_gulp that fits is " +
            std::to_string(rounded) + " (or reduce dm_max)");
    }
    cfg.nsamps_gulp = rounded;
    out.gulp_used   = rounded;
    out.lowered     = true;
    out.estimate    = estimate(cfg.nsamps_gulp);
    return out;
}

SizeType available_device_memory(dmt::Exec exec) {
#ifdef FROST_ENABLE_GPU
    if (exec.backend != dmt::Backend::kCPU)
        return device::available_memory(exec.backend, exec.device);
#else
    (void)exec;
#endif
    return 0;
}
MemoryCheck check_host_memory(config::PipelineConfig& cfg,
                              const psrio::BlockSource& source,
                              bool gulp_is_default,
                              SizeType available_bytes) {
    auto host_cfg          = cfg;
    host_cfg.dedisp.device = dmt::Backend::kCPU;
    const auto result =
        check_memory(host_cfg, source, gulp_is_default, available_bytes);
    cfg.nsamps_gulp = host_cfg.nsamps_gulp;
    return result;
}

} // namespace frost::pipeline
