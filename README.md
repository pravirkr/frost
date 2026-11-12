<h1 align="center">frost</h1>

<p align="center"><b>FDMT-accelerated Radio Optimal Search for Transients</b></p>

<div align="center">

[![Documentation Status](https://readthedocs.org/projects/frost/badge/?version=latest)](https://frost.readthedocs.io/en/latest/?badge=latest)
[![GitHub CI](https://github.com/pravirkr/frost/actions/workflows/ci.yml/badge.svg)](https://github.com/pravirkr/frost/actions/workflows/ci.yml)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-20-blue)](https://en.cppreference.com/w/cpp/20)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

</div>

**`frost`** searches channelised radio intensity data for fast radio bursts
and single pulses. One command reads a filterbank file or a live PSRDADA
stream, excises RFI, dedisperses every DM trial with the Fast Dispersion
Measure Transform, matched-filters every pulse width, and writes a clean,
clustered list of candidates.

**[Documentation](https://frost.readthedocs.io)** ·
[Installation](https://frost.readthedocs.io/en/latest/getting_started/installation.html) ·
[First search](https://frost.readthedocs.io/en/latest/getting_started/quickstart.html) ·
[CLI reference](https://frost.readthedocs.io/en/latest/reference/cli.html)

## ✨ Highlights

- **One command.** Every capability is an option of `frost`, every option
  has a default, and a TOML file can hold a survey's settings.
- **Inputs.** Sigproc filterbank, Breakthrough Listen HDF5 (one beam of a
  multi-beam file) and live PSRDADA ring buffers on Linux, through
  [psrio](https://github.com/pravirkr/psrio).
- **RFI excision.** Narrow- and broad-band excision by default; channel
  moments, IQRM, Kadane runs, dropouts, zero-DM, zdot and kill files when
  you need them.
- **Exact search.** Streaming FDMT with boxcar or trapezoid filters through
  [dmt](https://github.com/pravirkr/dmt), with the exact noise of every DM
  and width, including masked channels.
- **Sub-band search.** The full band, or also its halves and quarters, for
  band-limited bursts, merged into one row per event.
- **Fits the machine.** A memory plan before the first gulp, stage
  profiles, and a `run.toml` record of every run.
- **CPU first.** OpenMP on the CPU; CUDA and HIP backends are experimental.

## ⚡ Quickstart

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

## 📦 Installation

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

## 📜 Citing

A paper describing frost is in preparation. Until then, please cite the
software ([`CITATION.cff`](CITATION.cff)) and the FDMT paper,
[Zackay & Ofek (2017)](https://doi.org/10.3847/1538-4357/835/1/11).

## 🛠️ Development

[`AGENTS.md`](AGENTS.md) and [`docs/architecture.md`](docs/architecture.md)
describe the repository and its rules, [`docs/decisions.md`](docs/decisions.md)
the settled design choices, and [`docs/roadmap.md`](docs/roadmap.md) planned
work and known limitations. Changes are listed in
[`CHANGELOG.md`](CHANGELOG.md).

## 📄 License

frost is released under the [MIT License](LICENSE).
