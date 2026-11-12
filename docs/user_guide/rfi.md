# RFI excision

Before searching, frost cleans each gulp and calibrates every channel to
zero mean and unit noise. Two strategies are on by default; the others are
opt-in, each with its own switch.

| Strategy | Finds | Default | Switch |
|---|---|---|---|
| Narrow-band excision | single bright samples in one channel | on | `--clean-narrow` / `--no-clean-narrow` |
| Broad-band excision | bright time samples across the band at DM 0 | on | `--clean-broad` / `--no-clean-broad` |
| Dead channels | channels that never change | always | — |
| Static masks | channels you name | none | `--zap-chans`, `--zap-freqs`, `--kill-file` |
| Dropouts | blanked or dropped spectra | off | `--rfi-dropout` |
| Channel moments | channels with outlying statistics | off | `--rfi-moments` |
| IQRM | channels with outlying noise (Morello et al. 2022) | off | `--rfi-iqrm` |
| Kadane along time | weak, long-lived narrow-band RFI (Men & Barr 2024) | off | `--rfi-kadane-channel` |
| Kadane across frequency | wide-band bursts of RFI in one spectrum (Men & Barr 2024) | off | `--rfi-kadane-spectrum` |
| Zero-DM subtraction | common-mode RFI (Eatough et al. 2009) | off | `--rfi-zero-dm subtract` |
| zdot | common-mode RFI with per-channel gain (Men et al. 2019) | off | `--rfi-zero-dm zdot` |
| Equalisation | uneven channel levels | off | `--rfi-equalize` |

For dirty data a good first step beyond the default is channel masking:

```bash
frost --file obs.fil --dm-max 1000 --rfi-moments --rfi-iqrm --rfi-mask-grow 1
```

## Calibration

frost splits each gulp into windows of `--baseline-length` seconds
(default 2). In each window it measures every channel's level and noise
with a robust estimator that ignores outliers, and rescales the channel to
zero mean and unit noise. Masked channels are set to zero and excluded from
the search's noise model, so masking channels does not bias the S/N of the
remaining band.

## Default strategies

**Narrow-band excision** flags single samples more than `--rfi-tol`
standard deviations (default 5) from their channel's level. It is skipped
for data with 4 or fewer bits, where a few quantisation levels make it
meaningless.

**Broad-band excision** averages the cleaned band at DM 0, removes a
running baseline, and flags time samples whose band average is more than
`--rfi-tol` standard deviations out. `--rfi-broad-boxcar-max N` also
searches that series with boxcars up to `N` samples wide, to catch wider
broad-band RFI (default 1, single samples only).

`--rfi-tol` sets the threshold of both.

## Replacing flagged data

`--rfi-fill` chooses what replaces flagged samples:

- `neighbour` (default): a flagged sample takes the value of a random clean
  sample in a nearby channel at the same time; a flagged time sample takes,
  in each channel, the value at a random nearby clean time.
- `mean`: the channel's level.
- `noise`: Gaussian noise at the channel's level and noise.

Random choices depend only on `--rfi-seed`, so a run is reproducible and
does not depend on the number of threads.

## Static masks

To mask channels you already know are bad:

- `--zap-chans START END` masks channels `START` to `END`, inclusive and
  0-based. Repeat it for several ranges.
- `--zap-freqs LO HI` masks every channel within `LO` to `HI` MHz. Repeat it
  for several ranges. Prefer it for masks reused across observations with
  different channel counts.
- `--kill-file FILE` reads a sigproc kill file: one line per channel in the
  file's stored channel order, `1` to keep and `0` to kill. Blank lines and
  `#` comments are allowed. The file must list exactly as many channels as
  the input, and any other content is an error, so a file for a different
  receiver fails instead of masking the wrong channels.

```bash
frost --file obs.fil --zap-chans 0 15 --zap-chans 1000 1023 --zap-freqs 1420 1421
```

Channel numbers count channels as frost reads them, which `--fswap`
reverses. A kill file is mirrored to match, so one file serves both.

## Opt-in strategies

Their thresholds are set in a [configuration file](configuration.md) under
`[rfi.<strategy>]`.

**Dropouts** (`--rfi-dropout`) flags spectra whose spread across channels
is below `min_std_fraction` of the gulp's typical spread, plus one sample
either side: blanked or dropped packets.

**Channel moments** (`--rfi-moments`) computes each channel's mean,
standard deviation, skewness, excess kurtosis and lag-1 autocorrelation
over the gulp, and masks a channel when any of them lies more than
`threshold` inter-quartile ranges outside the middle half of all channels.
Each statistic can be switched off; `kurtosis_max` adds an absolute limit
on kurtosis. Masks hold for the whole gulp.

**IQRM** (`--rfi-iqrm`, Morello et al. 2022) compares each channel's noise
with its neighbours' up to `radius` channels away and masks the outliers.

`--rfi-mask-grow N` widens every channel masked by moments or IQRM by `N`
channels on each side. Static masks are not grown.

**Kadane along time** (`--rfi-kadane-channel`, Men & Barr 2024) finds, in
each channel, the run of samples with the largest summed deviation, and
flags it if it is significant above `threshold` and lasts at least
`min_duration` seconds. It catches weak, slow narrow-band RFI that a
per-sample threshold misses.

**Kadane across frequency** (`--rfi-kadane-spectrum`) finds, in each
spectrum, the most significant run of adjacent channels, and flags it if it
spans at least `min_bandwidth` MHz. When the run covers more than
`whole_band_fraction` of the band, the whole spectrum is flagged.

## Transforms

**Zero-DM filtering** (`--rfi-zero-dm subtract`, Eatough et al. 2009)
subtracts the band average from every channel at each time. **zdot**
(`--rfi-zero-dm zdot`, Men et al. 2019) subtracts a per-channel fit of the
band average instead, which also removes common-mode RFI that reaches
channels with different gains.

```{warning}
Both remove all signal at DM 0, so candidates near DM 0 become
meaningless. They also cost S/N for low-DM, wide pulses that sweep the band
in less than their own width.
```

**Equalisation** (`--rfi-equalize`) rewrites every channel to a common
level and noise.

Transforms need data with at least 8 bits per sample.

## Short gulps

The final gulp of a file can be short. A gulp of fewer than 64 spectra is
too short to measure channel statistics, so it reuses the calibration and
masks of the previous gulp. An input that is that short from the start is
calibrated with its own mean and noise, and transforms are skipped.

## Developer details

The algorithms, their statistics and the order of the passes are described
in [`docs/rfi.md`](https://github.com/pravirkr/frost/blob/main/docs/rfi.md)
in the repository.
