# RFI excision (`prepare::rfi::RFICleaner`)

The RFI stage sits between the data source and dedispersion. Every gulp,
`Pipeline` supplies time-major rows of `nchans` packed samples.
`RFICleaner::execute_normalized` decodes and cleans an owned float copy,
calibrates each channel/window, and returns channel-major float32 with
contiguous time and low frequencies first, as required by FDMTBoxcar.
Masked channels and padded samples are zero; padding never contributes
to the calibration. The default integer GPU path performs cleaning and
normalization on device; other configurations use the host cleaner.

Float32 input is validated as raw IEEE integer representations before it
is decoded or any RFI statistics are measured. NaN/Inf throws an error
naming the sample within the gulp and its channel. Input bytes remain
unchanged; no automatic replacement is applied. Padding is excluded from
validation as well as statistics.

The byte-cleaning `execute` API remains available for in-place cleaning
and applies the same float32 input validation. It publishes a per-channel
mask for callers that need it.

Every strategy has its own switch. By default narrow-band and broad-band
excision are enabled, with random-neighbour replacement; everything else
is opt-in. In the byte-cleaning API, unflagged samples are left bit-exact
unless a transform (`zero_dm`, `equalize`) is enabled.

`RfiConfig::validate()` checks all strategies, including disabled ones.
Thresholds must be positive and finite; minimum duration, bandwidth and
absolute kurtosis limits must be finite and nonnegative. Dropout and
whole-band fractions must lie in `[0, 1]`. Frequency zap bounds must be
finite and ordered, channel ranges ordered, and the broad-band maximum
boxcar at least one. Pipeline and CPU/device cleaner construction share
these checks. Cleaner construction and memory planning share observing
metadata validation, including representable packed/normalized block sizes
and baseline-to-sample conversion before allocation or integer casts.

## Strategies

| Strategy | Reference | Level | Default | Config |
|---|---|---|---|---|
| Static zaps by channel | — | channel | none | `rfi.zap_chans` |
| Channel kill file | sigproc 0/1 kill-file format | channel | none | CLI `--kill-file` (merged into `rfi.zap_chans`) |
| Static zaps by frequency | — | channel | none | `rfi.zap_freqs` |
| Dead channels | — | channel | always | — |
| Narrow-band excision | — | cell | on | `[rfi.narrow]` |
| Broad-band excision | — | time | on | `[rfi.broad]` |
| Dropouts | — | time | off | `[rfi.dropout]` |
| Spectral moments | — | channel | off | `[rfi.moments]` |
| IQRM | Morello et al. 2022 | channel | off | `[rfi.iqrm]` |
| Mask growth | — | channel | 0 | `rfi.mask_grow` |
| Kadane along time | Men & Barr 2024 | cell segment | off | `[rfi.kadane_channel]` |
| Kadane across frequency | Men & Barr 2024 | cell span / time | off | `[rfi.kadane_spectrum]` |
| Zero-DM subtraction | Eatough et al. 2009 | transform | off | `rfi.zero_dm = "subtract"` |
| zdot | Men et al. 2019 | transform | off | `rfi.zero_dm = "zdot"` |
| Equalisation | — | transform | off | `rfi.equalize` |

Static zaps (channel ranges, frequency ranges or a kill file) are removed
from every statistic and filled (see "Replacement"); they are not merely
skipped by dedispersion.

### Channel-level strategies

- **Dead channels.** A channel with zero variance in the bandpass sample
  of every window, and constant over the whole block, is masked.
  Channels whose variance is zero only in some windows are *unmeasured*
  in those windows. There they are left untouched, take no part in any
  statistic, and are never flagged. This avoids erasing, say, a
  noiseless synthetic pulse that the sample happened to miss.
- **Spectral moments.** For each channel, frost computes these
  statistics of the robustly normalised samples
  `z = (x - loc) / scale`: mean, std, skewness, excess kurtosis and
  lag-1 autocorrelation. They are aggregated over the block. A channel is
  masked if any selected statistic lies more than `threshold`
  inter-quartile ranges outside `[Q1, Q3]` across channels. With
  `kurtosis_max > 0`, a channel is also masked if its `|excess kurtosis|`
  exceeds that absolute limit. Spectral kurtosis finds non-Gaussian
  (bursty or CW) channels; autocorrelation finds slowly varying ones.
- **IQRM** (Morello et al. 2022). IQRM runs on each channel's standard
  deviation in data units, following Morello et al. (2022):
  - lagged differences at lags ±1, ±2, ±3, ±4, ±6, ... up to `radius`
    (default `nchans/10`);
  - a one-sided Tukey rule, `(d - median) > threshold * IQR / 1.349`;
  - a vote in which a channel is flagged when some channel that flagged
    it was itself flagged by fewer channels.

  Masked and unmeasured channels are ignored. `iqrm_mask` is also a public
  free function and is tested on deterministic lagged-difference cases.
  Once a lag reaches the channel span, every larger lag repeats the same
  edge-clamped comparisons, so frost stops after the first such lag; the
  votes are identical and extreme radii cannot overflow the lag loop.
- **Mask growth.** Widens every moments/IQRM-masked channel by
  `mask_grow` channels each side, inclusively and clipped to the channel
  span even for maximum-size radii. Static zaps are not grown.

### Time-level strategies

- **Broad-band excision.** The detection series is
  the *clipped* band mean `mean_c clamp(z, ±narrow.threshold)` over good
  channels at DM 0. Clipping means a single hot channel can't fake a
  broad-band event, and a masked channel can't contribute at all. A
  running-median baseline of radius `baseline/(2 dt)` is removed, the
  series is normalised by its MAD, and samples with `|x| > threshold` are
  flagged at boxcar widths 1, 2, 4, ... up to `boxcar_max`, each detection
  widened by its boxcar.

  The MAD is the exact median absolute value over the entire
  baseline-subtracted series, divided by 0.67448975; even lengths use the
  midpoint of the two central values. The host cleaner and the host
  flagging phase of device RFI share this estimator.
- **Dropouts.** Flags spectra whose spread across channels is below
  `min_std_fraction` of the block's median spread, plus the sample either
  side. These are blanked or dropped packets, and the rows a
  digitiser/packetiser zeroes.
- **Kadane across frequency, whole-band hits.** See the Kadane-across-
  frequency entry under "Cell-level strategies".

### Cell-level strategies

- **Narrow-band excision.** A cell is flagged when
  `|z| > threshold`, where `z` uses the channel's own robust level and
  rms (below). Skipped for data with 4 or fewer bits, where a few-level
  quantiser makes it meaningless.
- **Kadane along time** (Men & Barr 2024). For each channel, frost finds
  the maximum-sum run along time of `u = (z - clip_mean) / clip_std`
  (outliers count as 0), for both signs. A run is flagged if
  `sum / sqrt(len) > threshold` and it lasts at least `min_duration`
  seconds. This finds weak, long-lived narrow-band RFI (steps, slow ramps)
  that per-sample thresholds miss. At most one run per sign per channel
  per block is found.
- **Kadane across frequency** (Men & Barr 2024). For each time sample,
  frost finds the maximum-sum run across channels of the clipped `z`, for
  both signs. The cells are flagged if `sum / sqrt(n_good) > threshold`
  and the run spans at least `min_bandwidth` MHz. If the run covers more
  than `whole_band_fraction` of the band, the whole time sample is flagged
  instead.

### Transforms

- **Zero-DM subtraction** (Eatough et al. 2009) subtracts, from every
  kept cell, the band mean of the non-outlier `z` at that time.
- **zdot** (Men et al. 2019) subtracts a per-channel least-squares fit
  `a_c s(t) + b_c` of that band-mean series `s(t)` instead. The fit is over
  non-outlier cells. zdot removes common-mode RFI properly when channels
  respond with different gains; plain subtraction leaves `(1 - gain_c)`
  of it behind.
- **Equalisation** rewrites every channel to a common level and rms:
  0 and 1 for float data, and `2^(nbits-1)` and `2^nbits / 16` for
  integer data (128 and 16 for 8-bit).

Both zero-DM modes remove the DM-0 signal by construction, so DM ≈ 0
candidates become meaningless. They also cost S/N for low-DM, wide pulses
whose sweep across the band is shorter than the pulse. Transforms need at
least 8-bit data; this is rejected at construction.

## Replacement

Flagged data is replaced according to `fill`:

- `neighbour` (default):
  - A flagged **cell** takes the value of a random *clean* cell within
    `±max(nchans/60, 1)` channels at the same time, rescaled from the
    source channel's noise units into its own.
  - A flagged **time sample** takes, per channel, the value at a random
    unflagged time within `±max(baseline/(8 dt), 1)` samples.
  - Up to 10 draws are made; if none lands on a clean source, the cell
    falls back to the channel's local mean. Fallbacks are counted in
    `RfiReport::fill_fallbacks`.
- `mean`: the channel's local bandpass level.
- `noise`: Gaussian noise at the channel's local level and rms.

Masked channels are filled with their local mean, or with noise when
`fill = "noise"` in the byte-cleaning API. The normalized FDMTBoxcar
handoff zeros them, leaves active channels at unit noise, and passes the
gulp's channel mask to dmt, which excludes masked channels from each
plane/DM/width's analytical variance. Controlled tests measure unit noise sigma and no excess
blank-control detections with a half-band mask; correlations introduced by
the transforms are open items in the [roadmap](roadmap.md).

With a zero-DM mode on, refilled time samples are rebuilt in noise units
and have their own band mean removed. The final rounding error is carried
across channels, so each refilled spectrum cancels across the band like
every kept one does. Otherwise these samples would stand out in the (now
flat) DM-0 series.

Random draws use a counter-based hash of `(seed, block index, purpose,
time, channel, attempt)`. Output is therefore deterministic for a given
seed and independent of the OpenMP thread count.

## Dataflow and shared statistics

All enabled strategies share one set of statistics, computed in a fixed
small number of passes over the packed block:

1. **Bandpass.** For each window of `baseline_length` seconds, frost
   samples up to 3125 evenly spaced spectra. It seeds each channel's level
   and rms with a remedian/MAD estimate (Rousseeuw & Bassett 1990), then
   refines them with two 4.5σ-clipped mean/std iterations. The refinement
   matters for integer data with an rms of a few counts: the MAD is itself
   quantised there and can be ~30% off, which would turn a 5σ threshold
   into ~3.7σ.
2. **Moments.** Runs only if moments, IQRM or Kadane-along-time is on.
   One pass, parallel over (window × 64-channel blocks), accumulates raw
   and clipped moments and the lag-1 product. These feed the moment
   tests, IQRM's std and Kadane-along-time's normalisation, then the
   channel mask is built.
3. **Row scan.** One pass over every row, split into fixed chunks that
   depend only on `nsamps`. For each row it computes:
   - the detection and keep band means (broad-band detector, zero-DM);
   - the spectral spread (dropouts);
   - Kadane across frequency.

   It also accumulates the per-chunk zdot sums and Kadane-along-time
   states. Kadane states are an associative monoid, so chunks combine
   exactly in order.
4. **Flags.** The time-series detectors run on the scan's outputs, and
   the zdot fit and Kadane segments are resolved.
5. **Apply.** First, unflagged rows are processed in parallel. Each row
   is read, its cell states resolved, cells filled and transforms
   applied. Then the flagged rows are refilled from the final unflagged
   rows.

Per-stage timings and counts are in `RfiReport`, and are logged per gulp
at `-vv`.

## Channel kill file

`--kill-file PATH` reads a static channel mask in the sigproc kill-file
convention: one `1` (keep) or `0` (kill) per line, one line per channel, in
the input file's **stored** channel order. Blank lines and `#` comments are
ignored. Parsing is strict: anything other than a lone `0` or `1` on a
line, or a channel count different from the input's, is an error, so a
file made for another receiver or channelisation fails rather than masking
the wrong channels. The killed channels are merged into `rfi.zap_chans`,
so opt-in provenance records the effective ranges.

`zap_chans` index the channels as delivered to the pipeline, which `--fswap`
reverses; the kill file is mirrored accordingly, so one file serves both.
For masks reused across observations with different channelisations, prefer
`--zap-freqs`, which is independent of channel order and count.

## Configuration

TOML (`PipelineConfig::from_toml`). The zap ranges are examples (the
default is none); every other value shown is the default:

```toml
[rfi]
zap_chans = [[0, 10], [250, 255]]    # inclusive channel ranges
zap_freqs = [[1420.0, 1421.0]]       # inclusive MHz ranges
fill      = "neighbour"              # neighbour | mean | noise
zero_dm   = "off"                    # off | subtract | zdot
equalize  = false
mask_grow = 0
seed      = 24301                    # 0x5EED

[rfi.narrow]
enabled   = true
threshold = 5.0      # also the outlier clip for zero-DM and Kadane

[rfi.broad]
enabled    = true
threshold  = 5.0
boxcar_max = 1       # widest zero-DM boxcar searched

[rfi.dropout]
enabled          = false
min_std_fraction = 0.1

[rfi.moments]
enabled      = false
threshold    = 4.0   # IQRs outside [Q1, Q3]
mean         = true
std          = true
skewness     = true
kurtosis     = true
autocorr     = true
kurtosis_max = 0.0   # 0 disables the absolute test

[rfi.iqrm]
enabled   = false
threshold = 3.0
radius    = 0        # 0 = nchans / 10

[rfi.kadane_channel]
enabled      = false
threshold    = 7.0
min_duration = 0.05  # seconds

[rfi.kadane_spectrum]
enabled             = false
threshold           = 7.0
min_bandwidth       = 10.0   # MHz
whole_band_fraction = 0.8
```

CLI flags on `frost`:

| Flag | Effect |
|---|---|
| `--clean-narrow` / `--no-clean-narrow` | `narrow.enabled` |
| `--clean-broad` / `--no-clean-broad` | `broad.enabled` |
| `--rfi-tol` | sets both `narrow.threshold` and `broad.threshold` |
| `--rfi-broad-boxcar-max` | `broad.boxcar_max` |
| `--zap-chans START END`, `--zap-freqs LO HI` | static zaps (both repeatable) |
| `--kill-file PATH` | channel kill file, merged into `zap_chans` (see "Channel kill file" above) |
| `--rfi-fill`, `--rfi-seed` | `fill`, `seed` |
| `--rfi-zero-dm`, `--rfi-equalize` | transforms |
| `--rfi-dropout`, `--rfi-moments`, `--rfi-iqrm`, `--rfi-kadane-channel`, `--rfi-kadane-spectrum` | enable a strategy (each has a `--no-` form) |
| `--rfi-mask-grow N` | `mask_grow` |

Thresholds other than the narrow/broad pair are set through TOML.

## Caveats and known limits

- The bandpass is piecewise constant per window, not interpolated. A
  bandpass drifting fast within a window inflates the rms estimate.
- Kadane along time finds one segment per sign per channel per block.
  With the default `min_duration`, a genuine wide pulse in a single
  channel isn't at risk, but very long low-level RFI may need a longer
  `min_duration` or a lower threshold.
- The moment and IQRM statistics are aggregated over the whole block
  (gulp), so a channel is masked for the whole block.
- Byte-cleaning blocks shorter than 64 spectra are left untouched because
  robust channel statistics are unreliable at that size. After a reliable
  normalized block, short tails reuse its last window's raw/output
  calibration, channel mask and zdot regression. Zero-DM/zdot and equalization
  still apply; automatic time/cell excision and mask estimation do not.
  `RfiReport::calibration_reused` identifies this path. The device integer
  path also retains the last window and mask. Padding is always zero and
  excluded. A cold start without a preceding calibration still uses the
  short block's own mean/std and skips transforms; a pulse can bias that
  fallback. Retained calibration assumes gain remains stable into the tail.
  Byte-cleaning calls invalidate the host normalized calibration cache.
- Default integer neighbour cleaning and normalization run on CUDA/HIP;
  the other strategies use the host cleaner. Runtime validation of the
  FDMTBoxcar GPU path is pending (see the [roadmap](roadmap.md)).

## References

- Eatough, R. P., Keane, E. F., & Lyne, A. G. 2009 — zero-DM filtering.
- Men, Y., et al. 2019 — zdot filtering.
- Men, Y., & Barr, E. 2024, A&A, 683, A183 — Kadane-based RFI searches in
  time and frequency.
- Morello, V., et al. 2022 — IQRM channel masking.
- Rousseeuw, P. J., & Bassett, G. W. 1990 — the remedian, a robust
  averaging method for large data sets.
