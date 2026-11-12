# Architecture

This is the authoritative description of how the frost repository is
organised and the rules every change must follow. A change that needs to
break a rule updates this file in the same change.

## Layout

frost is one C++20 library (`frost::frost`) and one command-line
application (`applications/frost.cpp`, target `frost_app`, installed as
`frost`). FDMT, matched filtering and peak finding belong to
[dmt](https://github.com/pravirkr/dmt); input readers belong to
[psrio](https://github.com/pravirkr/psrio).

```text
include/frost/<domain>/*.hpp    installed public API, identical on CPU/CUDA/HIP
lib/
  common/                       bit-level finiteness validation
  config/                       configuration parsing and validation
  dedisp/                       DM trial and FDMT plan construction
  output/                       candidate writer, provenance
  pipeline/                     orchestration, input, memory, profile, benchmark
  search/                       clustering, merging, rate limiting, sub-band dedup
  prepare/                      private RFI interface and shared packed codecs
  cpu/*_cpu.cpp, *.hpp          host numerical plans and CPU kernels
  cuda/*_cuda.cu, *.cuh         CUDA/HIP implementations and toolkit helpers
applications/frost.cpp          the single CLI
cmake/
  CPM.cmake                     vendored CPM 0.43.1, byte-identical to upstream
  frostBuildInfo.cmake          build metadata header generation
  frostCompilerOptions.cmake    host compiler policy and coverage
  frostGPU.cmake                GPU toolchain selection and options
  frostDependencies.cmake       pinned dependencies
  frostInstall.cmake            installation and package exports
scripts/
  check_architecture.sh         mechanical boundary and header checks
  ci_static_analysis.sh         pre-commit, architecture and clang-tidy runner
  ci_lint_job.sh                parallel ci-tidy build and static analysis
tests/
  cpp/api/                      public API and CLI tests
  cpp/internal/                 white-box tests and device kernels
  cpp/support/                  shared fixtures
  cpp/data/                     local recordings, gitignored
  consumer/                     external public-target smoke project
docs/
  architecture.md, decisions.md developer documents, not published
  rfi.md, roadmap.md
  conf.py, requirements.txt     Sphinx site for Read the Docs (.readthedocs.yaml)
  index.md, getting_started/,   user documentation of the frost CLI
  user_guide/, reference/
  logo-src/make_logo.py         logo generator; writes _static/logo/
environment.yml                 conda environment with the system dependencies
CHANGELOG.md                    release notes (Keep a Changelog)
CITATION.cff                    software citation
```

## Boundaries

- Public headers are included as `<frost/...>`. They never include private
  headers, CUDA, HIP or Thrust, and never change declarations through
  backend macros. Existing public include paths are preserved.
- Private headers are included by quoted repository-root paths
  (`"lib/cpu/rfi_kernels.hpp"`, `"lib/cuda/gpu_utils.cuh"`). Shared private
  helpers stay in their domain; CPU and GPU code share representations
  through `lib/prepare/` without including each other's backend headers.
- The private include root is a `PRIVATE` requirement of the library.
  White-box tests get it through `frost::internal`, which is never installed
  or exported. Downstream users link `frost::frost` only.
- API tests never include `lib/...` headers or use backend macros.
- `scripts/check_architecture.sh` (also run with `SKIP_COMPILE=1` in
  pre-commit and the `architecture` CTest case) enforces these rules and
  compiles every public header standalone when a build directory is present.

## Code

**Public API** (`include/frost/`): C++20, `std::span` on the host and
`dmt::DeviceSpan` for device memory, the concepts in
`frost/common/policies.hpp` and `frost::DataType` instead of ad-hoc bit
depths. Errors are exceptions (`std::runtime_error`,
`std::invalid_argument`). Public classes carry a short comment on intent.

**Inner kernels** (`lib/cpu/`, `lib/cuda/`, private `lib/<domain>/*.hpp`):
as low-level as the hot path needs (raw pointers, `__restrict__`, OpenMP,
lookup tables). Do not hand-optimise code that is not hot.

- Parallelism is OpenMP on the CPU and kernels in `lib/cuda/*.cu` for the
  one selected GPU toolchain. No `std::execution`, TBB or other runtimes.
- Device code is additive behind the private `FROST_ENABLE_GPU` macro and
  never replaces the CPU path.
- Every pipeline component is a plan: a class constructed once (allocation,
  validation, planning) and driven by `execute()` per gulp or call.
- A class with a `std::unique_ptr<Impl>` declares, and defines out of line,
  its destructor and move operations. Copy is disabled unless needed.
- `Pipeline` constructs `dmt::algorithms::FDMTBoxcar` directly. No
  dedispersion wrapper or engine hierarchy.
- CPU-only plans are implemented directly in `lib/cpu/`, without a
  forwarding facade.
- New user-facing capability is a CLI11 option on `frost`, never a new
  executable, and is documented in the user docs (see Documentation). No
  Python bindings or Python dependency; building, testing and running
  frost never need Python. The documentation build and the logo generator
  (`docs/logo-src/make_logo.py`) are the only exceptions.
- A new stage has its public header in `include/frost/<domain>/`, backend-
  neutral code in `lib/<domain>/`, and `lib/cpu/<name>_cpu.cpp` /
  `lib/cuda/<name>_cuda.cu` when needed.

**Large components.** An `Impl` that grows past about 1000 lines is split
into a few files grouped by adjacent stages, sharing one private header that
declares the whole `Impl`. Templates called across translation units have
explicit instantiations in their defining file and `extern template` in the
header. CUDA relocatable device code is not used: each `.cu` file keeps its
kernels together with the `Impl` methods that launch them, and `__device__`
helpers shared across files are `inline` in a `.cuh`.

**Naming** (`.clang-tidy` `readability-identifier-naming`): types
`CamelCase`; functions, variables, members and namespaces `lower_case`;
private/protected members `m_`; constants `kCamelCase`; macros
`UPPER_CASE`. Include order: own header, standard library, third-party,
`<frost/...>`, private `"lib/..."`, local test helpers.

## Numerics

Release builds use `-O3 -ffast-math` and, by default, `-march=native`.

- Finiteness is checked on integer bit patterns (`frost::is_finite`), never
  with `std::isfinite`. Sample bytes and configuration values are validated
  at ingress, before any floating-point load; NaN/Inf is rejected, never
  repaired. Only finite, calibrated data reaches dmt.
- Expressions that must be exactly zero for equal inputs use
  contraction-safe forms (`(a - b) * (a + b)`, not `a*a - b*b`).
- Integer sizes and counts never go through `std::pow` or `std::log`.
- Variances are centred and accumulated in double.
- Integer tolerances and geometry saturate or are validated before use.
- Arguments are validated before any plan state or output changes.
- Exact-float test assertions only target values exact by construction.
  Run Release tests: these bugs often hide in Debug.

## Data contract with dmt

- frost passes float32 shaped `(1, nchans, nsamps)`, time contiguous per
  channel, frequency ascending. Descending input is reversed during
  normalisation, and masks are reversed with it.
- Active channels have zero mean and unit noise; masked channels are zero;
  dmt's `noise_sigma` is one.
- Each gulp's channel mask goes to `FDMTBoxcar::execute(waterfall,
  kill_mask)`. Static zaps are the constructor mask; a gulp whose mask
  equals it passes an empty span.
- Result views are consumed before the next engine call.

## Memory

- `memory_limit_mib` / `--memory-mib` limits only the selected backend's
  pool (host RAM on CPU, device memory on CUDA/HIP); zero means automatic.
  GPU host buffers are reported, not enforced. Units are MiB (2^20 bytes).
- An explicit limit is passed to dmt as `max_memory_bytes`.
- The budget is checked before constructing `FDMTBoxcar` or allocating large
  workspaces. Planning uses FDMT plan metadata and never constructs an
  engine to measure it.
- An explicit gulp that does not fit fails; the default gulp may shrink but
  stays longer than the maximum delay plus the widest boxcar.
- All candidate storage is bounded. Exceeding pending capacity throws;
  engine overflow warns once and is counted. Candidates are never silently
  dropped.
- Estimates are reported separately from measured capacities (`-v`,
  `--profile`).

## Configuration and provenance

- TOML parsing is strict. `PipelineConfig::validate()` runs from both the
  CLI and the `Pipeline` constructor.
- Every configuration field round-trips through `to_toml_string` /
  `from_toml_string`; a new field updates the all-fields round-trip fixture.
- Provenance (`--provenance`, `run.toml`) is opt-in, owned by the CLI, and
  records effective settings after overrides and gulp adjustment.

## Dependencies

Pinned in [`cmake/frostDependencies.cmake`](../cmake/frostDependencies.cmake).
No new third-party dependency without discussion first.

- System: OpenMP, HDF5, single-precision FFTW.
- CPM-pinned: dmt, psrio, PSRDADA (Linux, `FROST_WITH_PSRDADA`), fmt,
  spdlog, HighFive, xsimd, toml++, CLI11, Catch2. Local checkouts are opt-in
  only (`-DCPM_dmt_SOURCE=/path/to/dmt`).
- Installs fetch dependencies over the network (`decisions.md`).
  `environment.yml` lists the system dependencies; CI does not test it.
- `cmake/CPM.cmake` is the unmodified CPM release, never downloaded at
  configure time; dmt and psrio reuse it through `CPM_PATH`, which their
  bootstraps check against the same SHA-256. Update it by replacing the file
  with a newer release; never edit it.
- `cmake --install` installs the `frost` CLI only (plus the shared
  `libpsrdada` it links on Linux). `FROST_INSTALL_LIBRARY` adds libfrost,
  the public headers and the CMake package; a shared build
  (`BUILD_SHARED_LIBS`) always does, since the CLI links it. With the
  library, `FROST_INSTALL_BUNDLED_DEPS` (default on) also runs the install
  scripts of the fetched packages the installed config needs: dmt, psrio,
  fmt and PSRDADA. A dependency newly exposed through public headers or
  `frostConfig.cmake` joins that list in `frostDependencies.cmake`. CI
  checks both installs: the CLI alone, and the library with the consumer
  project.
- frost behaves as a subproject: tests (`FROST_BUILD_TESTING`) and install
  rules (`FROST_INSTALL`) default to `PROJECT_IS_TOP_LEVEL`, and the build
  type and CPM settings it chooses never override a parent's cache.
- Optional: CUDA ≥ 12.2 or ROCm/HIP ≥ 6.2.
- Logging is spdlog; formatting is `fmt::format`.

## Build and test

[`CMakePresets.json`](../CMakePresets.json) is the source of truth for
configure flags; machine-specific settings go in an ignored
`CMakeUserPresets.json`. Sources are globbed with `CONFIGURE_DEPENDS`, so
reconfigure after adding or removing files. Never commit build output.

```bash
cmake --preset cpu && cmake --build --preset cpu && ctest --preset cpu
```

- Every change keeps `-DFROST_GPU=OFF` building and all CPU tests passing.
- Every new component gets Catch2 tests: `tests/cpp/api/` for the public
  contract, `tests/cpp/internal/` for private helpers. CTest names start
  with `api:` or `internal:`; GPU and parity tests are tagged `[gpu]` /
  `[parity]`.
- Tests assert intended behaviour with independent oracles, not whatever
  the implementation produces. When a synthetic fixture does not trigger
  the expected behaviour, suspect the fixture first.
- Data-dependent tests (`[data]`) read local recordings from
  `tests/cpp/data/` (gitignored) and skip when they are absent.
- Format changed files only: `pre-commit run clang-format --files <files>`
  (clang-format 23.1.1). clang-tidy 23 runs from the `ci-tidy` preset
  (`scripts/ci_static_analysis.sh`); every suppression names a check and
  gives a reason.

## Documentation

- The published site (Sphinx with MyST Markdown, built by Read the Docs
  from `.readthedocs.yaml`) documents the `frost` CLI: installation, usage,
  every option and the `.cand` format. It has no API reference; the public
  headers document the library, and `docs/reference/library.md` covers
  linking it.
- The developer documents in `docs/` (this file, `decisions.md`,
  `roadmap.md`, `rfi.md`) are excluded from the site in `docs/conf.py`.
  Site pages link to them by GitHub URL.
- A new or changed option updates `docs/reference/cli.md` and the user
  guide page it belongs to. The `api:` CTest case `[cli][docs]` fails when
  `frost --help` and `docs/reference/cli.md` disagree.
- The site builds without warnings:
  `sphinx-build -W -b html docs docs/_build/html` with the packages pinned
  in `docs/requirements.txt`. Read the Docs builds it, so it costs no CI
  minutes.
- The logo, tagline lockup and favicon in `docs/_static/logo/` are generated
  by `uv run docs/logo-src/make_logo.py` from pinned, hash-checked fonts.
  Change the script, not the SVGs.
- `README.md` is a landing page: what frost is, a quickstart, a short
  install and links into the site. Details belong in the site.

## Changes

- Work on a branch and open a pull request against `main`.
- Update [`decisions.md`](decisions.md) when a frozen choice changes and
  [`roadmap.md`](roadmap.md) when planned work or limitations change.

## Versions and releases

- `project(VERSION)` in `CMakeLists.txt` is the only version. The build
  info, `frost::build_info()`, `frost --version`, the shared library's
  `VERSION`/`SOVERSION` and `frostConfigVersion.cmake` all follow it.
- Before 1.0 any minor release may break the API and ABI: the soname is
  `MAJOR.MINOR` and `find_package(frost X.Y)` accepts only `X.Y.*`. From 1.0
  both follow the major version.
- The installed config requires at least the pinned dmt and psrio versions
  (`FROST_DMT_VERSION`, `FROST_PSRIO_VERSION`).
- A release bumps `project(VERSION)` (and `version` in `CITATION.cff`) on
  `main` and moves `CHANGELOG.md`'s Unreleased entries under the new
  version, then tags that commit `vX.Y.Z` and publishes it as a GitHub
  release. There are no separate
  tarballs or binaries (see [`decisions.md`](decisions.md)).
