# Roadmap

Open issues and planned work for frost. Settled choices are in
[`decisions.md`](decisions.md).

## Science and correctness

### Sub-band trials factor from pure noise

`--subband-levels` 1 and 2 search 4 and 11 planes at one `--detect-thresh`.
Run signal-free observations (the controlled blank controls first, then real
off-source data) at depths 0, 1 and 2, and measure raw and final candidate
rates per plane and in total as a function of threshold. Use the result to
choose between a documented single threshold, per-depth thresholds that hold
the total false-alarm rate, or a trials-corrected significance column.

### Kill-mask follow-ups

- **Recovery across changing masks.** Measure injection recovery at an equal
  false-alarm rate when the RFI mask changes every gulp. dmt scores the first
  `S + W - 1` endpoints after a change with the OR-mask variance; a
  measurable loss there would justify exact transition variances in dmt.
- **GPU validation** of the masked device path, and of `drain()` on a GPU
  engine: on the skewed-window path it allocates dmt's host-input staging
  block at end of input, outside the memory budget check.
- **Pre-flight memory estimate** for dmt's mask noise tables and staging,
  which dmt only charges at the first masked gulp. The current planner does
  not include them.
- Pending candidates can grow for as long as an excursion stays open;
  `max_pending_candidates` still fails explicitly. Decide whether long
  excursions need a splitting policy.

### Calibration

- **Calibration policy.** None of the tested estimated policies (current
  window, lagged, interpolated) is a universal replacement. Next, test
  longer or adaptive history with gain-stability checks, reliable cold
  start, and parameter-estimation uncertainty, against real recordings and
  RFI, using several independent blank-control partitions. Keep defaults
  unchanged until that evidence exists.
- **Cold starts and changing gains.** Cold starts below 64 samples use the
  block's own mean/std and skip transforms. On a gain ramp, DM-80 pulses
  come out about 7% low in S/N. An untracked tail gain doubling gives noise
  sigma 1.976 and 33 blank-control candidates versus 1 with known gain.
  Validate bandpass interpolation on reliable windows and define a policy
  for unmeasured tails and cold starts before claiming uniform sensitivity.
- **Gulp-boundary S/N.** A pulse within about `boxcar_max / 2` of an emit
  edge gets gulp-dependent S/N and can leave weak extra rows. Cross-gulp
  candidate carry handles later-closing widths; the remaining work is in
  the per-gulp calibration windows.

### Correlated noise and candidate localisation

- Zero-DM/zdot marginal normalization does not restore the independent-noise
  model. Controlled AR(1) input gives width-16 noise sigma 2.547 and about
  1600 blank-control candidates across 16 runs. Mean-filled spectra have
  zero noise; neighbour and noise replacement have different correlation
  properties. Extend the study to measured receiver statistics, combined
  drift and correlation, unmeasured windows and real recordings before
  changing defaults.
- Some raw sub-band matches end with grouped coordinates outside the truth
  gate although their strongest S/N is retained. Strongest-member
  coordinates fix six of seven such cases; in the seventh (masked) case the
  strongest member itself lies outside the gate, and an RFI stress case has
  the reverse. Review grouping uncertainty and both sub-band dedup modes
  with these fixtures.
- **Default RFI on red-noise-heavy data.** With narrow/broad excision,
  clustering can absorb a real injected pulse into a nearby red-noise event;
  moments+IQRM recover it. Compare both at a matched false-alarm rate on
  more real data before revisiting the default.
- `.cand` has no absolute arrival-time column. Run provenance (`run.toml`)
  records the start time but is not a substitute.

### Short-input summary and capacity

On `tests/cpp/data/benchmark_J0540_decimated.fil` (9765 samples) with
`--dm-max 1000`, `frost` prints `searched 0` yet writes 28 candidates, and
warns that FDMTBoxcar dropped 934973 candidates globally. Find out whether
the summary's searched count misses samples searched at end of input
(`drain()`), and why a file this short overflows the candidate capacity.

### Validation before performance or sensitivity claims

Record executable revisions, full effective configuration, input hashes,
truth catalogues, hardware, compiler flags and raw results. Choose
acceptance bounds before running.

1. **Recovery at matched false-alarm rate.** Paired injected and
   signal-free recordings over a grid in DM, fluence, width, fractional
   bandwidth, spectral shape, scattering and intra-channel smearing.
   Include quantization, masked channels, drift and correlation, real RFI,
   both channel orders, different gulp alignments and end of input. Report
   binomial uncertainty and misses. Select thresholds on separate controls
   and account for sub-band trials.
2. **Significance and localisation.** Null distributions per
   DM/width/plane, false events per observing hour after full reduction,
   S/N relative to a known-noise reference, and time, DM, width and band
   errors for both coordinate sets.
3. **Throughput and bounded resources.** End-to-end sustained samples/s,
   latency distribution, peak host/device memory and behaviour under
   candidate storms, including reading, cleaning, transfers, clustering and
   output, with warm-up and repeated trials.
4. **Operational behaviour.** Long streams, short final gulps, malformed
   input, output failure, memory and pending-capacity exhaustion, and
   restarts. Validate the installed library consumer and CLI.

## GPU validation

`FROST_GPU=AUTO|CUDA|HIP|OFF` selects the toolchain. FDMTBoxcar and the
default integer RFI path run on the device; other RFI strategies calibrate
on the host; candidate processing and output stay on the host.

- Run the `[gpu][parity]` pipeline and normalization tests on CUDA hardware
  for the current FDMTBoxcar path, including warm tails, masks and `drain()`.
- Renew compute-sanitizer coverage on the current device path.
- Build and test CUDA (12.2 minimum) and HIP by hand on real hardware (CI
  stays CPU-only, see `decisions.md`); HIP has not been compiled yet.
- Share CPU/GPU behaviour fixtures so parity tests exercise the same
  contracts on both backends.
- Move toward a shared RFI execution core for host and device, so that
  strategies beyond the default integer path do not need a separate device
  implementation each.
- Re-measure GPU throughput on the fused engine.

## Performance

Measure before optimising; none of these is known to be the current
bottleneck on the fused engine.

- Host clustering of large red-noise candidate counts per gulp is a
  significant cost of GPU runs at `--subband-levels 2`. Device clustering
  or a DM-binned neighbour search would remove it.
- `SubbandDedupPlan` compares all pairs per gulp, and within-plane
  clustering is quadratic inside a dense time window (a broadband burst at
  every DM and width). Windowing on aligned time would bound both.
- GPU runs use pageable host copies and do not overlap the next gulp's
  FDMT/RFI with the current search. Non-default host RFI uploads a block
  per gulp.
- The bandpass stage (remedian seed plus clipped refinement) is about 70%
  of default cleaning time.
- xsimd is already a dependency but unused. Apply it to a CPU kernel only
  after that kernel is measured to be hot.

## Packaging

frost installs from source over the network: CPM fetches the pinned
dependencies, `environment.yml` provides the system ones, and a plain
`cmake --install` installs the CLI (`FROST_INSTALL_LIBRARY` adds a
self-contained library prefix). frost also works as a CPM subproject. The
user documentation is a Sphinx site for Read the Docs. Still to do:

- **First release.** Tag `v0.1.0` and publish it as a GitHub release.
- **Reproducible pins.** `URL_HASH` on every downloaded dependency archive;
  dmt pinned by commit like psrio; a more reliable source for PSRDADA than
  SourceForge git.
- **Read the Docs pull-request builds.** Enable them on the `frost` project,
  so documentation changes are checked without CI minutes.
- **GitHub social preview.** Upload `social-preview.png` from
  `uv run docs/logo-src/make_logo.py --exports` in the repository settings.
- **Untested install paths.** CI runs neither shared-library builds
  (`frostInstall.cmake` and `frostConfig.cmake.in` have shared-only paths)
  nor the `add_subdirectory`/CPM consumer path, and `environment.yml` is
  checked only by hand (on macOS). Cover them in the existing CI jobs where
  that costs few minutes; otherwise check by hand before a release.

## Future capabilities

### Multi-beam coincidencing

Match candidates across beams as a mode of the `frost` binary, reusing
`search::are_coincident`. This is a different problem from in-pipeline
sub-band association, which matches frequency planes of one beam; do not
fold the dispersion-delay shift into the cross-beam match. It needs a
network transport and a multi-beam candidate-buffering design. An offline
mode that merges several beams' `.cand` files and flags events seen in many
beams as RFI would cover most offline use. Multi-beam RFI rejection belongs
here too.

Running several beams in one process would need an explicit OpenMP thread
budget and serialized FFTW plan creation. Until then, run one `frost -k`
per PSRDADA key (one beam per key).

### Baseband search

psrio reads GUPPI RAW through `psrio::formats::guppi::RawReader`, but frost
does not yet channelise, dedisperse or search voltages. This needs design
work: coherent versus incoherent dedispersion, channelisation (PFB) and
detection, what RFI excision means for voltages, and whether the intensity
pipeline is reused after detection. Write a design before starting.

### Injection mode

Promote the test fixture `tests/cpp/support/fake_filterbank.hpp` into an
`--inject` mode: inject a grid of pulses (DM, width, fluence) into real
data and report completeness and S/N recovery. The current controlled study
is a regression fixture, not a user injection mode or a survey completeness
estimate.

### Candidate cutouts

For each merged candidate, write the dedispersed time series and a
dedispersed dynamic-spectrum cutout to HDF5 (via the existing HighFive
dependency), for machine-learning classifiers and human vetting. Cutouts
come before a richer candidate format.

### Richer candidate output

The first nine `.cand` columns follow the common single-pulse layout,
followed by frequency/channel extents, strongest-member S/N and extent, and
strongest-member coordinates. An optional extended format could add MJD, DM
range, width in seconds and a per-candidate RFI score.

### Further RFI methods

- Generalised spectral kurtosis on averaged spectra, once the accumulation
  and shape metadata are available and quantization and prior processing
  are accounted for. The existing temporal excess-kurtosis channel test is a
  different statistic.
- An interpolated bandpass instead of a per-window step.
- A switchable lagged calibration and/or per-channel streaming exponential
  baseline filter. First measure burst self-fitting versus gain-tracking
  lag, long-pulse attenuation and covariance with matched blank and
  injection controls.
- Per-sub-block channel masks instead of per-gulp masks.
- More of the RFI suite on the device: moments, IQRM, Kadane, dropout, noise
  fill, zero-DM, zdot and equalisation currently run on the host.

### Input and workflow

- PSRFITS input.
- A downsampling workflow.

## Longer term

- Floating-point per-channel variance weights and masks that change within
  a gulp (dmt extensions).
- A noise model that accounts for time/channel covariance introduced by
  RFI transforms, replacement and red noise.
- Allocation-free execution-memory estimates from dmt, replacing frost's
  conservative public-metadata bounds. Until then, check frost's planning
  estimates against dmt's reported memory usage.
