# Design decisions

The design choices frost starts from. They are settled; changing one is a
deliberate decision recorded here in the same change.

## Search

- dmt's `FDMTBoxcar` does streaming FDMT (Zackay & Ofek 2017), analytical
  matched filtering and peak finding. Candidates keep dmt's absolute sample
  indices.
- `--subband-levels 0|1|2` searches 1, 4 or 11 planes: the full band, then
  the halves and quarters with staggered bands across their boundaries.
  Non-power-of-two channel counts split top-down into balanced halves.
- One user-provided `--detect-thresh` applies to every plane at every depth.
- Peaks are reduced per excursion per width (`per_width`) by default.
- Boxcar filters are the default; trapezoids (`--trapezoid`) are opt-in.

## Noise model and masks

- Every active channel is calibrated to zero mean and unit noise; masked
  channels are zero.
- Channel masks change the noise model inside dmt, per gulp and per row.
  There is no global rescaling of active channels.
- Unfinished candidates are held until dmt's candidate frontier minus
  `(cand_sep_time + 1) * boxcar_max`. End of input calls `drain()`.

## RFI and calibration

- Default RFI is narrow- and broad-band excision with neighbour fill.
  Channel moments and IQRM (Morello et al. 2022) are opt-in. Every strategy
  has its own switch.
- Narrow-band excision uses each channel's robust bandpass and rms.
  Statically zapped channels are excluded from every statistic and filled.
- The broad-band detector's scale is the exact MAD of the full series.
- A channel is dead only if it has zero variance in every window and is
  constant over the block. Zero variance in some windows means unmeasured
  there: left untouched and never flagged.
- Each channel and window is calibrated from its own robust fit (remedian
  seed, Rousseeuw & Bassett 1990, then clipped refinement).
- Blocks shorter than 64 spectra reuse the last reliable calibration, mask
  and zdot coefficients.
- Kill files are strict sigproc 0/1 files: one line per channel in stored
  order, `#` comments allowed, any other token or a channel-count mismatch
  is an error. They merge into `zap_chans` and are mirrored under `--fswap`.
- Non-finite input is rejected, never repaired.

## Candidates

- The optional rate limit applies per plane, after the engine.
- Within a plane, candidates are clustered friends-of-friends on time, DM
  trial and width, with a physical DM tolerance
  `cand_sep_dm_smear * widest_width * dm_step(band)`, then a seeded pass
  lets bright clusters absorb weaker DM wings. Members must share plane and
  channel extent.
- Every member of an unfinished group is kept across gulps, bounded by
  `max_pending_candidates`; overflow is an error.
- Sub-band association is transitive by default (`--subband-dedup
  seeded` is selectable). The written row keeps the widest band's
  coordinates and appends the strongest member's.
- `.cand` files have 23 tab-separated columns; the first nine follow the
  common single-pulse layout.

## Packaging

- Installs are network installs: CMake fetches the pinned dependencies
  through CPM at configure time. Offline builds and builds against system
  copies of those dependencies are not supported install paths
  (`FROST_USE_SYSTEM_DEPS` stays a developer option), and frost is not
  published to a package-manager channel (conda-forge, Spack,
  distributions).
- frost is for users of the `frost` CLI. `cmake --install` installs the CLI
  alone; libfrost, its headers and the CMake package are opt-in
  (`FROST_INSTALL_LIBRARY`) for the rare advanced user. There are no Python
  bindings, and none are planned.
- `environment.yml` is the one convenience: a conda environment with the
  system dependencies (compilers, CMake, HDF5, FFTW, OpenMP), for a local
  build and `cmake --install` into that environment.
- A release is a `vX.Y.Z` git tag matching `project(VERSION)`, published as
  a GitHub release. There are no separate source tarballs or binaries.
- User documentation is a Read the Docs site about the CLI, with no API
  reference. `README.md` is a short landing page that links into it.
- CI minutes are scarce: CI builds and tests the CPU code on Linux runners
  only. No CUDA, HIP, conda or container jobs; those paths are checked by
  hand.

## Platform

- One helper derives the frequency axis for both dedispersion and band
  extents.
- On GPU runs, `FDMTBoxcar` and the default RFI path run on the device;
  other RFI strategies run on the host and upload; candidate processing and
  output stay on the host.
