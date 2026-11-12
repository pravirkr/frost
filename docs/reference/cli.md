# CLI reference

```text
frost [OPTIONS]
```

Pass exactly one of `--file`, `--dada-key` or `--benchmark`. Every other
option has a default. Options override the values of a `--config` file;
the *Key* column names the [configuration file](../user_guide/configuration.md)
key each one sets. Each switch also has a negated form, such as
`--no-fswap`, which is useful to override a configuration file.

`frost --help` prints the same list. A test in frost's suite checks that
every option it prints is documented on this page.

## General

| Option | Default | Key | Description |
|---|---|---|---|
| `-h`, `--help` | | | Print the options and exit. |
| `--version` | | | Print the version, source revisions and build settings, and exit. |
| `--config FILE` | | | Load settings from a TOML file. Command-line options override it. |
| `-v`, `--verbose` | warnings only | `verbosity` | `-v` logs memory planning and one line per gulp; `-vv` adds stage timings. |
| `--benchmark` | | | Run the fixed synthetic benchmark and print its profile. |

## Input

| Option | Default | Key | Description |
|---|---|---|---|
| `--file PATH` | | `input.sigproc_file` | Sigproc (`.fil`) or Breakthrough Listen HDF5 (`.h5`, `.hdf5`) filterbank. |
| `--input-beam N` | `0` | `input.beam` | 0-based beam of a multi-beam HDF5 file. Ignored otherwise. |
| `-k`, `--dada-key KEY` | | `input.dada_key` | PSRDADA ring-buffer key, hexadecimal, with or without `0x` (Linux). |
| `--fswap`, `--no-fswap` | off | `input.fswap` | Reverse the channel order, searching the mirrored DM sweep. |

See [Inputs](../user_guide/inputs.md).

## Output

| Option | Default | Key | Description |
|---|---|---|---|
| `--output-dir DIR` | `.` | `output.output_dir` | Directory for `.cand` files and `run.toml`; created if needed. |
| `--per-gulp-files`, `--single-file` | single file | `output.per_gulp_files` | Write one `<utc>_<beam>.cand` per gulp instead of one `candidates.cand`. |
| `--beam N` | from the input | `output.beam` | 1-based beam number in per-gulp file names. |
| `--provenance`, `--no-provenance` | off | `output.provenance` | Write the effective settings and run metadata to `run.toml`. |
| `--profile`, `--no-profile` | off | `profile` | Print the stage profile at the end of the run. |

See [Candidates](../user_guide/candidates.md) and [Memory and
performance](../user_guide/resources.md).

## Gulps and memory

| Option | Default | Key | Description |
|---|---|---|---|
| `--nsamps-gulp N` | `262144` | `nsamps_gulp` | Samples per gulp. The default may be shortened to fit memory; an explicit value is not. |
| `--memory-mib N` | `0` (automatic) | `memory_limit_mib` | Memory budget in MiB: host RAM on a CPU run, device memory on a GPU run. |
| `--async-io`, `--no-async-io` | on | `async_io` | Read the next gulp while searching the current one. |
| `--baseline-length S` | `2.0` | `baseline_length` | Calibration and RFI window, seconds. |

## Device

| Option | Default | Key | Description |
|---|---|---|---|
| `--device NAME` | `cpu` | `dedisp.device` | `cpu`, `cuda` or `hip`. |
| `--dedisp-backend NAME` | | `dedisp.device` | Alias of `--device`. |
| `--device-id N` | `0` | `dedisp.device_id` | GPU ordinal for `cuda` or `hip`. |

See [GPU runs](../user_guide/gpu.md).

## RFI excision

| Option | Default | Key | Description |
|---|---|---|---|
| `--clean-narrow`, `--no-clean-narrow` | on | `rfi.narrow.enabled` | Narrow-band (per-sample) excision. |
| `--clean-broad`, `--no-clean-broad` | on | `rfi.broad.enabled` | Broad-band (DM 0, per time sample) excision. |
| `--rfi-tol SIGMA` | `5.0` | `rfi.narrow.threshold`, `rfi.broad.threshold` | Threshold of both narrow- and broad-band excision. |
| `--rfi-broad-boxcar-max N` | `1` | `rfi.broad.boxcar_max` | Widest DM-0 boxcar the broad-band excision searches. |
| `--zap-chans START END` | | `rfi.zap_chans` | Mask channels `START` to `END`, inclusive. Repeatable. |
| `--zap-freqs LO HI` | | `rfi.zap_freqs` | Mask channels within `LO` to `HI` MHz, inclusive. Repeatable. |
| `--kill-file FILE` | | | Sigproc kill file: one `1` (keep) or `0` (kill) per channel, in stored order. Merged into `rfi.zap_chans`. |
| `--rfi-fill MODE` | `neighbour` | `rfi.fill` | Replacement for flagged data: `neighbour`, `mean` or `noise`. |
| `--rfi-seed N` | `24301` | `rfi.seed` | Seed for random replacement. |
| `--rfi-zero-dm MODE` | `off` | `rfi.zero_dm` | Zero-DM filter: `off`, `subtract` or `zdot`. |
| `--rfi-equalize`, `--no-rfi-equalize` | off | `rfi.equalize` | Rewrite every channel to a common level and noise. |
| `--rfi-dropout`, `--no-rfi-dropout` | off | `rfi.dropout.enabled` | Flag blanked or dropped spectra. |
| `--rfi-moments`, `--no-rfi-moments` | off | `rfi.moments.enabled` | Mask channels with outlying mean, noise, skewness, kurtosis or autocorrelation. |
| `--rfi-iqrm`, `--no-rfi-iqrm` | off | `rfi.iqrm.enabled` | Mask channels flagged by IQRM on their noise. |
| `--rfi-mask-grow N` | `0` | `rfi.mask_grow` | Widen moments and IQRM masks by `N` channels on each side. |
| `--rfi-kadane-channel`, `--no-rfi-kadane-channel` | off | `rfi.kadane_channel.enabled` | Flag long-lived runs along time in each channel. |
| `--rfi-kadane-spectrum`, `--no-rfi-kadane-spectrum` | off | `rfi.kadane_spectrum.enabled` | Flag wide runs across frequency in each spectrum. |

The thresholds of the opt-in strategies are set in a configuration file.
See [RFI excision](../user_guide/rfi.md).

## Dedispersion

| Option | Default | Key | Description |
|---|---|---|---|
| `--dm-min DM` | `0.0` | `dedisp.dm_min` | Lowest DM searched, pc cm⁻³. |
| `--dm-max DM` | `1000.0` | `dedisp.dm_max` | Highest DM searched, pc cm⁻³. |
| `--dm-tol F` | `1.25` | `dedisp.dm_tol` | Allowed smearing growth between DM trials; above 1. |
| `--dm-pulse-width US` | `40.0` | `dedisp.dm_pulse_width` | Intrinsic pulse width assumed for DM spacing, microseconds. |
| `--dm-nbits N` | `32` | `dedisp.dm_nbits` | Bits per dedispersed sample. FDMT writes float32, so only 32 is accepted. |

## Search

| Option | Default | Key | Description |
|---|---|---|---|
| `--boxcar-max N` | `4096` | `search.boxcar_max` | Widest matched filter, samples (at most 4096). |
| `--wtsp F` | `2.0` | `search.wtsp` | Width spacing: 2 doubles, 1 tries every width, 1.5 is geometric. |
| `--trapezoid`, `--no-trapezoid` | off | `search.use_trapezoid` | Use trapezoids (half-weight edges) for widths of 3 and more. |
| `--detect-thresh SNR` | `6.0` | `search.detect_thresh` | Detection threshold, S/N. |
| `--peak-mode MODE` | `per_width` | `search.peak_mode` | Peaks per above-threshold run: `per_width`, `across_widths` or `all`. |
| `--subband-levels N` | `0` | `search.subband_levels` | `0` full band; `1` also the halves (4 planes); `2` also the quarters (11 planes). |
| `--max-candidates N` | `131072` | `search.max_candidates` | Detections kept per engine call. |
| `--max-cands-per-dm N` | `4096` | `search.max_cands_per_dm` | Detections kept per plane and DM trial. |
| `--max-giant-rate R` | `0` (off) | `search.max_giant_rate` | Cap on detections per minute of data, per plane. |

See [The search](../user_guide/search.md).

## Clustering

| Option | Default | Key | Description |
|---|---|---|---|
| `--cluster`, `--no-cluster` | on | `search.cluster` | Cluster each plane's detections and merge sub-bands before writing. `--no-cluster` writes every detection. |
| `--cand-sep-time N` | `3` | `search.cand_sep_time` | Clustering tolerance in time, samples. |
| `--cand-sep-filter N` | `3` | `search.cand_sep_filter` | Clustering tolerance in width-trial index. |
| `--cand-sep-dm N` | `200` | `search.cand_sep_dm` | Clustering tolerance in DM trials. |
| `--cand-sep-dm-smear F` | `8.0` | `search.cand_sep_dm_smear` | Beyond adjacent trials, the DM tolerance in boxcar widths of smearing; `0` uses the trial count only. |
| `--subband-dedup MODE` | `transitive` | `search.subband_dedup` | How sub-band matches group: `transitive` or `seeded`. |
| `--max-pending-candidates N` | `0` | `search.max_pending_candidates` | Detections held across gulps for unfinished clusters; `0` uses `--max-candidates`. Exceeding it is an error. |

See [Candidates](../user_guide/candidates.md).

## Exit status

`frost` exits with 0 on success. A run that fails prints
`frost: error: ...` to standard error and exits with 1; with
`--provenance`, `run.toml` records the failure. An unknown option or an
invalid value is reported before the run starts, with a non-zero status.
