# Configuration files

Every setting can be kept in a TOML file and loaded with `--config`:

```bash
frost --config survey.toml --file obs.fil
```

Options on the command line override the file, so one file can hold a
survey's settings while each run names its input:

```bash
frost --config survey.toml --file obs.fil --detect-thresh 7
```

An input given on the command line replaces any input the file sets:
`--file` clears a `dada_key` from the file, and `--dada-key` clears a
`sigproc_file`.

## Rules

- A file may set any subset of keys; the rest keep their defaults.
- Parsing is strict. An unknown key, a value of the wrong type or a negative
  count is an error, so a misspelt key fails instead of being ignored.
- Values are checked together after the command line is applied: for
  example `dm_tol` must be above 1 and `boxcar_max` below `nsamps_gulp`.
- `--provenance` records the effective settings of a run in the
  `[config_effective]` table of `run.toml`, with the same keys (see
  [Memory and performance](resources.md#provenance)). To reuse them, copy
  that table's contents into a configuration file without the
  `config_effective.` prefix on the table names.

## Every key and its default

```toml
# Top level
nsamps_gulp      = 262144   # samples per gulp
baseline_length  = 2.0      # seconds; RFI and calibration window
memory_limit_mib = 0        # MiB for the selected backend; 0 = automatic
verbosity        = 0        # 0 warnings, 1 per-gulp info, 2+ debug
async_io         = true     # read the next gulp while searching this one
profile          = false    # print the stage profile at the end

[input]
sigproc_file = ""           # sigproc or HDF5 filterbank
dada_key     = ""           # PSRDADA key (hex); exclusive with sigproc_file
beam         = 0            # 0-based beam of a multi-beam HDF5 file
fswap        = false        # reverse the channel order

[dedisp]
device         = "cpu"      # cpu | cuda | hip
device_id      = 0          # GPU ordinal
dm_min         = 0.0        # pc cm^-3
dm_max         = 1000.0     # pc cm^-3
dm_tol         = 1.25       # smearing tolerance between DM trials (> 1)
dm_pulse_width = 40.0       # expected intrinsic width, microseconds
dm_nbits       = 32         # FDMT writes float32; only 32 is accepted

[search]
boxcar_max             = 4096          # widest matched filter, samples (<= 4096)
wtsp                   = 2.0           # width spacing: 2 doubles, 1 every width
use_trapezoid          = false         # trapezoids for widths >= 3
detect_thresh          = 6.0           # S/N threshold
peak_mode              = "per_width"   # per_width | across_widths | all
max_candidates         = 131072        # detections per engine call
max_cands_per_dm       = 4096          # detections per plane and DM trial
max_pending_candidates = 0             # carried across gulps; 0 = max_candidates
cluster                = true          # cluster and merge before writing
cand_sep_time          = 3             # clustering tolerance in time, samples
cand_sep_filter        = 3             # clustering tolerance in width index
cand_sep_dm            = 200           # clustering tolerance in DM trials
cand_sep_dm_smear      = 8.0           # physical DM tolerance; 0 = trials only
max_giant_rate         = 0.0           # detections per minute per plane; <= 0 off
subband_levels         = 0             # 0, 1 or 2
subband_dedup          = "transitive"  # transitive | seeded

[output]
output_dir     = "."
beam           = 0          # 1-based beam in file names; 0 = from the input
per_gulp_files = false      # one <utc>_<beam>.cand per gulp
provenance     = false      # write run.toml

[rfi]
zap_chans = []              # inclusive channel ranges, e.g. [[0, 10], [250, 255]]
zap_freqs = []              # inclusive MHz ranges, e.g. [[1420.0, 1421.0]]
fill      = "neighbour"     # neighbour | mean | noise
zero_dm   = "off"           # off | subtract | zdot
equalize  = false
mask_grow = 0               # widen moments/IQRM masks by N channels
seed      = 24301           # seed for random replacement

[rfi.narrow]
enabled   = true
threshold = 5.0

[rfi.broad]
enabled    = true
threshold  = 5.0
boxcar_max = 1

[rfi.dropout]
enabled          = false
min_std_fraction = 0.1

[rfi.moments]
enabled      = false
threshold    = 4.0
mean         = true
std          = true
skewness     = true
kurtosis     = true
autocorr     = true
kurtosis_max = 0.0

[rfi.iqrm]
enabled   = false
threshold = 3.0
radius    = 0               # 0 = nchans / 10

[rfi.kadane_channel]
enabled      = false
threshold    = 7.0
min_duration = 0.05         # seconds

[rfi.kadane_spectrum]
enabled             = false
threshold           = 7.0
min_bandwidth       = 10.0  # MHz
whole_band_fraction = 0.8
```

The [CLI reference](../reference/cli.md) maps each option to its key. The
RFI keys are explained in [RFI excision](rfi.md); some of them (the
thresholds of the opt-in strategies, for example) can be set only in a file.
