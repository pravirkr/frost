#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <dmt/common/backend.hpp>
#include <dmt/common/boxcar_plans.hpp>
#include <dmt/common/candidate.hpp>

#include <frost/common/numeric.hpp>
#include <frost/common/policies.hpp>
#include <frost/dedisp/dedisp.hpp>
#include <frost/prepare/rfi.hpp>
#include <frost/search/subband_dedup.hpp>

namespace frost::config {

// Defaults follow a conventional single-pulse search setup: 262144-sample
// gulps, doubling boxcar widths, narrow- and broad-band excision and
// trial-count clustering tolerances.

struct InputConfig {
    // Sigproc `.fil` or a Breakthrough Listen HDF5 filterbank
    // (`.h5` / `.hdf5`). Empty when the input is a PSRDADA ring.
    std::string sigproc_file;
    // Hex PSRDADA ring-buffer key (no 0x required). Empty when the input
    // is a file. Mutually exclusive with `sigproc_file`.
    std::string dada_key;
    bool fswap = false;
    // 0-based beam/IF index for a multi-beam HDF5 filterbank. Ignored
    // by sigproc and PSRDADA. 0 reads the first beam.
    SizeType beam                             = 0;
    bool operator==(const InputConfig&) const = default;
};

// Every RFI strategy and its switch; see prepare/rfi.hpp and docs/rfi.md.
using RfiConfig = prepare::rfi::RfiConfig;

// nchans/dt/f0/df are intentionally absent here: they come from the
// psrio block source at runtime, from the input file rather than a config
// file.
struct DedispConfig {
    float dm_min         = 0.0F;
    float dm_max         = 1000.0F;
    float dm_tol         = 1.25F;
    float dm_pulse_width = 40.0F; // microseconds
    DataType dm_dtype    = DataType::kFloat32;
    // FDMT execution backend. Writes float32; adaptive DM scrunching is
    // not used. `device_id` is the GPU ordinal for cuda/hip (default 0)
    // and is ignored on the CPU.
    dmt::Backend device                        = dmt::Backend::kCPU;
    int device_id                              = 0;
    bool operator==(const DedispConfig&) const = default;
};

struct SearchConfig {
    SizeType boxcar_max = 4096;
    // Width spacing: 2 doubles the width, 1 searches every integer width,
    // 1.5 is a geometric compromise.
    float wtsp = 2.0F;
    // Search widths >= 3 with dmt's trapezoid (half-weight edges of
    // (W + 1) / 4 samples) instead of the boxcar. The analytical noise
    // model includes the taper; it helps pulses off the sample grid or with
    // soft edges, and costs about 5% S/N on aligned flat pulses.
    bool use_trapezoid = false;
    dmt::PeakFindingMode peak_mode =
        dmt::PeakFindingMode::kContiguousPeakPerWidth;
    SizeType max_candidates   = 131'072;
    SizeType max_cands_per_dm = 4096;
    // Retained members of unfinished clusters, not just their winners.
    // 0 uses max_candidates. Exceeding the carry bound fails explicitly.
    SizeType max_pending_candidates = 0;
    float detect_thresh             = 6.0F;
    SizeType cand_sep_time          = 3;
    SizeType cand_sep_filter        = 3;
    SizeType cand_sep_dm            = 200;
    // Physical DM tolerance for clustering and sub-band dedup, in boxcar
    // widths of dispersive smearing across the band. Beyond adjacent trials
    // two giants share a cluster only within that DM distance, so a fine
    // trial grid cannot make `cand_sep_dm` span everything. 0 uses the
    // trial count only.
    float cand_sep_dm_smear = 8.0F;
    float max_giant_rate    = 0.0F; // <=0 disables the rate limiter
    // true: within each FDMT plane, friends-of-friends on time, DM
    // trial and width (highest S/N), then deduplicate nested sub-band
    // extents along the DM curve (widest channel extent). false: write
    // every detection, with neither pass.
    bool cluster = true;
    // Depth 0 is full band only; higher depths include tree and staggered
    // bands across their boundaries (depth 1: 4 planes; depth 2: 11).
    SizeType subband_levels = 0;
    // How the sub-band dedup groups matches ("transitive" or "seeded"; see
    // `search::SubbandDedupMode`).
    search::SubbandDedupMode subband_dedup =
        search::SubbandDedupMode::kTransitive;
    bool operator==(const SearchConfig&) const = default;
};

struct OutputConfig {
    std::string output_dir = ".";
    // 1-based beam number for output naming; 0 takes it from the input.
    SizeType beam = 0;
    // false: one `candidates.cand` per run. true: one
    // `<gulp start UTC>_<beam>.cand` per gulp.
    bool per_gulp_files                        = false;
    bool provenance                            = false;
    bool operator==(const OutputConfig&) const = default;
};

struct PipelineConfig {
    InputConfig input;
    RfiConfig rfi;
    DedispConfig dedisp;
    SearchConfig search;
    OutputConfig output;
    // MiB, applied to host RAM on CPU or device memory on GPU; 0 is automatic.
    SizeType memory_limit_mib = 0;
    SizeType nsamps_gulp      = 262144;
    float baseline_length     = 2.0F; // seconds
    int verbosity             = 0;    // 0 warnings, 1 per-gulp info, 2+ debug
    // Read the next gulp on a side thread while the current one is searched.
    // One beam stays serial through FDMT; this only hides read latency.
    bool async_io = true;
    // Print the accumulated stage profile at the end of the run. The
    // profile is always collected; this flag only asks the CLI to print it.
    bool profile = false;

    /** Loads a `PipelineConfig` from a TOML file (see the [input]/[rfi]/
     * [rfi.<strategy>]/[dedisp]/[search]/[output] tables and top-level
     * nsamps_gulp/baseline_length/verbosity/async_io/profile keys). Fields
     * absent from the file keep their struct defaults above. Unknown keys
     * and wrong-typed or negative values throw. */
    static PipelineConfig from_toml(const std::string& path);
    static PipelineConfig from_toml_string(std::string_view text);

    /** Complete canonical TOML, including defaults and CLI overrides. */
    [[nodiscard]] std::string to_toml_string() const;

    /** Throws `std::invalid_argument` for values that would hang, crash or
     * silently misbehave downstream (non-finite numbers, `dm_tol <= 1`,
     * `boxcar_max >= nsamps_gulp`, ...). `from_toml` does not call it, so
     * a loaded file can still be overridden by the CLI first; `frost` and
     * `Pipeline` both do. Unknown TOML keys and wrong-typed values are
     * rejected earlier, in `from_toml`. */
    void validate() const;

    // Sanity ceilings that keep size arithmetic far from SizeType overflow.
    static constexpr SizeType kMaxBoxcar         = 4096;
    static constexpr SizeType kMaxGulp           = SizeType{1} << 31;
    bool operator==(const PipelineConfig&) const = default;
};

/** Maps a `dm_nbits` value (8, 16 or 32; 32 means float) to the
 * DMT output `DataType`. */
DataType dm_dtype_from_nbits(int nbits);

/** Parses a PSRDADA shared-memory key (`dada`, `0xdada`). */
std::uint32_t parse_dada_key(std::string_view text);

dmt::PeakFindingMode peak_mode_from_string(std::string_view s);
std::string to_string(dmt::PeakFindingMode mode);

/** Parse/format the RFI enums by their config names ("neighbour", "mean",
 * "noise"; "off", "subtract", "zdot"). */
prepare::rfi::FillMode fill_mode_from_string(std::string_view s);
prepare::rfi::ZeroDMMode zero_dm_from_string(std::string_view s);
std::string to_string(prepare::rfi::FillMode mode);
std::string to_string(prepare::rfi::ZeroDMMode mode);

} // namespace frost::config
