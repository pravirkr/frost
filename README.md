# FROST

**FROST** (FDMT-accelerated Radio Optimal Search for Transients) is a C++20
fast-radio-burst and single-pulse search pipeline for channelized intensity
data, with CPU, CUDA and HIP backends. It provides a library (`frost::frost`)
and one command-line application, `frost`.

## Features

- **Inputs** through [psrio](https://github.com/pravirkr/psrio): sigproc
  filterbank (`.fil`), Breakthrough Listen HDF5 filterbank (`.h5`/`.hdf5`,
  including one beam from a multi-beam cube) and live PSRDADA ring buffers
  (Linux, `-DFROST_WITH_PSRDADA=ON`).
- **Switchable RFI suite**: broad-band (zero-DM) and narrow-band excision,
  zero-DM subtraction or zdot, channel moments and IQRM masking, Kadane
  run searches along time and frequency, dropout detection, static channel
  and frequency zaps, and sigproc channel kill files. Each channel is
  calibrated to zero mean and unit noise before search. See
  [`docs/rfi.md`](docs/rfi.md).
- **Search** with [dmt](https://github.com/pravirkr/dmt): streaming FDMT
  (Zackay & Ofek 2017) with analytical boxcar or trapezoid matched filters
  and peak finding, on `dmt::Exec` (CPU, CUDA or HIP).
- **Sub-band search**: the full band plus half and quarter bands, including
  staggered overlapping bands (1, 4 or 11 planes).
- **Per-gulp channel kill masks**: masked channels are excluded from every
  plane's analytical noise model rather than rescaled globally.
- **Candidate post-processing**: per-plane rate limiting, friends-of-friends
  clustering within each plane with bounded cross-gulp retention, and
  association of nested sub-band detections along the dispersion curve.
- **Memory planning**: a pre-flight budget for the selected backend and
  optional run provenance.

## Requirements

- CMake ≥ 3.21 and a C++20 compiler (GCC ≥ 11 or Clang ≥ 17)
- OpenMP, HDF5 and single-precision FFTW (`libfftw3f`)
- Optional: CUDA ≥ 12.2 or ROCm/HIP ≥ 6.2

Other dependencies (dmt, psrio, PSRDADA, fmt, spdlog, HighFive, xsimd,
toml++, CLI11, Catch2) are fetched and pinned through CPM on first
configure; see [`cmake/frostDependencies.cmake`](cmake/frostDependencies.cmake).

## Build

With presets:

```bash
cmake --preset cpu            # Release, GPU off, tests on, in build/
cmake --build --preset cpu
ctest --preset cpu
```

Or with raw CMake:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFROST_GPU=OFF -DFROST_BUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`-DFROST_GPU=AUTO|CUDA|HIP|OFF` (default `AUTO`) selects at most one GPU
toolchain. `AUTO` tries CUDA, then HIP, and does not require a device;
explicit `CUDA` or `HIP` requires a visible device unless
`-DFROST_GPU_CHECK_DEVICE=OFF`. The `cuda` and `hip` presets build those
backends. Release builds use `-O3 -ffast-math` and, by default,
`-march=native`; the `cpu-portable` preset disables native
tuning, and `FROST_CPU_ARCH` selects an explicit target. Other presets
(`cpu-debug`, `release`, `ci`, `asan`, `coverage`, `ci-tidy`) are listed in
[`CMakePresets.json`](CMakePresets.json). Machine-specific settings belong
in an ignored `CMakeUserPresets.json`.

## Usage

```bash
# Search a filterbank file up to DM 1000, writing to out/
frost --file obs.fil --dm-max 1000 --output-dir out -v

# One beam of a multi-beam HDF5 file, on a CUDA GPU
frost --file obs.h5 --input-beam 0 --dm-max 1000 --device cuda --device-id 0

# Live PSRDADA ring buffer, one .cand file per gulp
frost -k dada --dm-max 2000 --per-gulp-files --output-dir out

# Sub-band search with trapezoid filters, channel kill file, moments+IQRM
# RFI masking, and a run.toml provenance record
frost --file obs.fil --subband-levels 2 --trapezoid \
      --kill-file chans.kill --rfi-moments --rfi-iqrm \
      --output-dir out --provenance

# Synthetic benchmark of the complete pipeline
frost --benchmark
```

`frost --help` lists every option. Commonly used ones:

| Option | Meaning |
|---|---|
| `--config file.toml` | Load a `PipelineConfig`; command-line flags take precedence |
| `--dm-min`, `--dm-max`, `--dm-tol` | DM range and trial spacing tolerance |
| `--boxcar-max`, `--wtsp` | Widest boxcar (default 4096) and width spacing (default 2) |
| `--trapezoid` | Search widths ≥ 3 with half-weight-edge trapezoids |
| `--detect-thresh` | Detection threshold in S/N (default 6) |
| `--subband-levels 0\|1\|2` | Search 1, 4 or 11 planes (full band; plus halves; plus quarters) |
| `--peak-mode` | `per_width` (default), `across_widths` or `all` |
| `--no-cluster` | Write every emitted peak without clustering or sub-band association |
| `--subband-dedup` | `transitive` (default) or `seeded` sub-band association |
| `--zap-chans START END`, `--zap-freqs LO HI`, `--kill-file F` | Static channel masks (zaps repeatable, inclusive) |
| `--rfi-*`, `--clean-broad`, `--clean-narrow` | RFI stage switches; see [`docs/rfi.md`](docs/rfi.md) |
| `--device cpu\|cuda\|hip`, `--device-id` | Execution backend and GPU ordinal |
| `--nsamps-gulp` | Samples per gulp (default 262144) |
| `--memory-mib`, `--max-pending-candidates` | Backend memory budget and candidate-retention limit |
| `--profile`, `--provenance` | Stage profile; `run.toml` provenance record |

A kill file lists one `1` (keep) or `0` (kill) per line, one line per
channel in the file's stored order; `#` comments are allowed and the file
must list exactly `nchans` channels.

## Output

Candidates are written to `<output-dir>/candidates.cand`, or to one
`<utc>_<beam>.cand` file per gulp with `--per-gulp-files`. Files are
tab-separated with no header row. The first nine columns follow the
conventional single-pulse candidate layout; the remaining fourteen are
frost band and association metadata:

| # | Column | Meaning |
|---|---|---|
| 1 | `snr` | Detection S/N |
| 2 | `samp_idx` | Peak sample index |
| 3 | `time` | Peak time (s), `samp_idx * tsamp` |
| 4 | `filter_idx` | Width-trial index |
| 5 | `dm_idx` | DM-trial index |
| 6 | `dm` | DM (pc cm⁻³) |
| 7 | `members` | Number of detections merged into this row |
| 8, 9 | `begin`, `end` | First and last above-threshold sample |
| 10, 11 | `f_low`, `f_high` | Frequency edges (MHz) of the band searched |
| 12, 13 | `chan_begin`, `chan_end` | Inclusive original-channel extent of that band |
| 14 | `snr_best` | S/N of the strongest associated sub-band row |
| 15, 16 | `chan_begin_best`, `chan_end_best` | Channel extent of that row |
| 17 | `n_extents` | Number of distinct channel extents associated |
| 18 | `samp_idx_best` | Strongest member's peak sample |
| 19 | `time_best` | Strongest member's peak time (s) |
| 20 | `dm_best` | Strongest member's DM |
| 21 | `width_best` | Strongest member's width (samples) |
| 22, 23 | `f_low_best`, `f_high_best` | Strongest member's frequency edges (MHz) |

Without sub-band association, columns 14-23 repeat the row's own values
(and `n_extents` is 1). Columns 18-23 give the strongest member's location
in its own frequency plane; for a narrow-band pulse they can differ from the
wide-band representative in columns 1-13.

## Status

GPU execution of the current search path has not yet been validated on
hardware, and the HIP backend is untested. Multi-beam coincidencing and
baseband search are planned; see [`docs/roadmap.md`](docs/roadmap.md).

## Documentation

- [`docs/architecture.md`](docs/architecture.md): repository layout and development rules
- [`docs/decisions.md`](docs/decisions.md): settled design choices
- [`docs/rfi.md`](docs/rfi.md): RFI stages and options
- [`docs/roadmap.md`](docs/roadmap.md): planned work and known limitations

## Citing frost

A paper describing frost is in preparation. Until it is available, please
cite this repository. Citation details will be added here.

## License

frost is released under the [MIT License](LICENSE).
