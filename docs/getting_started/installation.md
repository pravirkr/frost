# Installation

frost builds from source. Configuring needs network access, because CMake
fetches frost's pinned dependencies (dmt, psrio, fmt, spdlog and a few
others) the first time. Releases are `vX.Y.Z` tags on
[GitHub](https://github.com/pravirkr/frost/releases).

## Requirements

- A C++20 compiler: GCC 11 or newer, or Clang 17 or newer.
- CMake 3.21 or newer, and git.
- OpenMP, HDF5 and single-precision FFTW (`libfftw3f`).
- Optional, for GPU runs: CUDA 12.2 or newer, or ROCm/HIP 6.2 or newer. See
  [GPU runs](../user_guide/gpu.md).

Install the system packages first:

::::{tab-set}
:::{tab-item} Ubuntu / Debian
:sync: linux

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build git pkg-config libhdf5-dev libfftw3-dev
```

Ubuntu 22.04, Debian 12 and newer ship GCC 11 or later.
:::
:::{tab-item} macOS (Homebrew)
:sync: macos

Apple's Clang has no OpenMP, so build with Homebrew's LLVM:

```bash
brew install llvm libomp cmake ninja pkg-config hdf5 fftw
export CC="$(brew --prefix llvm)/bin/clang" CXX="$(brew --prefix llvm)/bin/clang++"
```
:::
:::{tab-item} conda (no root)
:sync: conda

For clusters and other machines without root access, or with a system
compiler older than GCC 11, `environment.yml` in the repository provides the
compilers, CMake, HDF5, FFTW and OpenMP from conda-forge:

```bash
git clone https://github.com/pravirkr/frost.git
cd frost
conda env create -f environment.yml   # or mamba / micromamba
conda activate frost
```

Then build as below, and install into the environment with
`--prefix "$CONDA_PREFIX"`.
:::
::::

## Build and install

```bash
git clone https://github.com/pravirkr/frost.git
cd frost
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFROST_BUILD_TESTING=OFF
cmake --build build -j
cmake --install build --prefix ~/.local
```

This installs one file, `~/.local/bin/frost` (on Linux also `libpsrdada`
beside it in `~/.local/lib`). Choose any prefix you like; make sure its
`bin/` is on your `PATH`.

Check the install:

```bash
frost --version
```

```text
frost 0.1.0
revision: 75be258400596081c3eccf98ff7ab7028edee6fb
dmt: 980e49464f8beeb8d91868dc0a8aee1b719fe298
psrio: b2d0da516580614bf8341924e51d153f48c4d9bb
compiler: Clang 23.1.5
build: Release, GPU off, arch native
```

Include this output in bug reports.

## Build options

Pass these to the first `cmake` command as `-DNAME=VALUE`.

| Option | Default | Effect |
|---|---|---|
| `FROST_GPU` | `AUTO` | GPU toolchain: `AUTO` (CUDA, then HIP, else none), `CUDA`, `HIP` or `OFF`. |
| `FROST_ENABLE_NATIVE_ARCH` | `ON` | Tune Release builds for this machine's CPU (`-march=native`). |
| `FROST_CPU_ARCH` | empty | An explicit `-march` target instead, such as `x86-64-v3`. |
| `FROST_WITH_PSRDADA` | `ON` on Linux | Live PSRDADA ring-buffer input. |
| `FROST_BUILD_TESTING` | `ON` | Build the test suite (run it with `ctest --test-dir build`). |

```{important}
A Release build tuned with `-march=native` may crash with an illegal
instruction on an older or different CPU. To build one `frost` for several
machines, configure with `-DFROST_ENABLE_NATIVE_ARCH=OFF` or set
`FROST_CPU_ARCH` to the oldest CPU you need.
```

To run the test suite before installing, leave `FROST_BUILD_TESTING` on and
run `ctest --test-dir build -j 8`. Run it on a Release build: in Debug the
tests are very slow.

## Updating

```bash
git pull
cmake --build build -j
cmake --install build --prefix ~/.local
```

CMake fetches new dependency pins on its own when `git pull` changes them.

## The C++ library

The default install is the CLI only. A few users embed frost in their own
C++ program; see [Using libfrost](../reference/library.md) for that.
