<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/_static/logo/frost-logo-tagline-dark.svg">
    <img alt="frost: Radio Transient Search" src="docs/_static/logo/frost-logo-tagline.svg" width="350">
  </picture>
</p>

<div align="center">

[![Documentation Status](https://readthedocs.org/projects/frost/badge/?version=latest)](https://frost.readthedocs.io/en/latest/?badge=latest)
[![GitHub CI](https://github.com/pravirkr/frost/actions/workflows/ci.yml/badge.svg)](https://github.com/pravirkr/frost/actions/workflows/ci.yml)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-20-blue)](https://en.cppreference.com/w/cpp/20)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

</div>

**FROST** (FDMT-accelerated Optimal Search for Radio Transients) is a C++20
fast-radio-burst and single-pulse search pipeline for channelised intensity
data, with CPU, CUDA and HIP backends. It provides a library (`frost::frost`)
and one command-line application, `frost`.

**[Documentation](https://frost.readthedocs.io)** ·
[Installation](https://frost.readthedocs.io/en/latest/getting_started/installation.html) ·
[First search](https://frost.readthedocs.io/en/latest/getting_started/quickstart.html) ·
[CLI reference](https://frost.readthedocs.io/en/latest/reference/cli.html)

## Features

- **Inputs.** Sigproc filterbank, Breakthrough Listen HDF5 (one beam of a
  multi-beam file) and live PSRDADA ring buffers on Linux, through
  [psrio](https://github.com/pravirkr/psrio).
- **RFI excision.** Narrow- and broad-band excision by default; channel
  moments, IQRM, Kadane runs, dropouts, zero-DM, zdot and kill files when
  you need them.
- **Search.** Streaming FDMT with boxcar or trapezoid filters through
  [dmt](https://github.com/pravirkr/dmt), with the exact noise of every DM
  and width, including masked channels.
- **Sub-band search.** The full band plus half and quarter bands, including
  staggered overlapping bands (1, 4 or 11 planes).
- **Candidate post-processing.** Per-plane rate limiting, friends-of-friends
  clustering within each plane with bounded cross-gulp retention, and
  association of nested sub-band detections along the dispersion curve.
- **Fits the machine.** A memory plan before the first gulp, stage
  profiles, and a `run.toml` record of every run.

## Quickstart

```bash
# Search a filterbank file up to DM 1000, writing to out/candidates.cand
frost --file obs.fil --dm-max 1000 --output-dir out

# A live PSRDADA ring, one .cand file per gulp, halves and quarters too
frost -k dada --dm-max 2000 --subband-levels 2 --per-gulp-files --output-dir out

# Dirtier data: mask channels with moments and IQRM, record the run
frost --file obs.fil --rfi-moments --rfi-iqrm --kill-file bad.kill --provenance
```

Each row of the `.cand` file is one candidate: S/N, sample, time, width,
DM and the band it was found in. See
[Candidates](https://frost.readthedocs.io/en/latest/user_guide/candidates.html).

## Installation

frost builds from source with CMake 3.21+, a C++20 compiler (GCC 11+ or
Clang 17+), OpenMP, HDF5 and FFTW. CMake fetches the other dependencies
over the network.

```bash
git clone https://github.com/pravirkr/frost.git && cd frost
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFROST_BUILD_TESTING=OFF
cmake --build build -j
cmake --install build --prefix ~/.local
frost --version
```

No root access or an old compiler? `environment.yml` provides everything
through conda. See the
[installation guide](https://frost.readthedocs.io/en/latest/getting_started/installation.html)
for system packages, GPU builds and build options.

## Citing

A paper describing frost is in preparation. Until then, please cite the
software ([`CITATION.cff`](CITATION.cff)) and the FDMT paper,
[Zackay & Ofek (2017)](https://doi.org/10.3847/1538-4357/835/1/11).

## Development

[`AGENTS.md`](AGENTS.md) and [`docs/architecture.md`](docs/architecture.md)
describe the repository and its rules, [`docs/decisions.md`](docs/decisions.md)
the settled design choices, and [`docs/roadmap.md`](docs/roadmap.md) planned
work and known limitations. Changes are listed in
[`CHANGELOG.md`](CHANGELOG.md).

## License

frost is released under the [MIT License](LICENSE).
