# The search

frost dedisperses each gulp with the Fast Dispersion Measure Transform
(FDMT, Zackay & Ofek 2017), convolves every DM trial with matched filters
of increasing width, and keeps peaks above a threshold. The search engine is
[dmt](https://github.com/pravirkr/dmt)'s `FDMTBoxcar`.

## DM trials

`--dm-min` and `--dm-max` (default 0 and 1000 pc cm⁻³) bound the search.
frost spaces the trials so that the S/N lost between two neighbouring
trials stays small:

- `--dm-tol` (default 1.25) is the allowed smearing growth between trials,
  as a factor. Larger values mean fewer, coarser trials. It must be above 1.
- `--dm-pulse-width` (default 40 µs) is the intrinsic pulse width assumed
  when computing that smearing.

Lower `--dm-tol` towards 1 for a finer grid; raise `--dm-max` for distant
sources. The number of trials grows with both, and so do run time and
memory.

## Widths

Each DM trial is convolved with filters 1, 2, 4, … samples wide, up to
`--boxcar-max` (default and maximum 4096):

- `--wtsp` sets the spacing of widths: 2 doubles each time, 1 tries every
  width, and values in between (1.5) give a geometric spacing.
- `--trapezoid` uses trapezoids, whose two edge samples carry half weight,
  for widths of 3 and more. They suit pulses that fall between samples or
  have soft edges, and cost about 5% S/N on flat pulses aligned with the
  samples. Boxcars are the default.

The noise of every DM and width is known exactly from the transform, so the
S/N of every trial is on the same scale without any measured correction.

## Threshold and peaks

`--detect-thresh` (default 6) is the S/N threshold, applied to every DM,
width and sub-band alike.

A pulse stays above threshold for several samples. `--peak-mode` chooses
which samples become detections:

- `per_width` (default): one peak per above-threshold run, for each width.
- `across_widths`: one peak per run, over all widths together.
- `all`: every sample above threshold. This can produce a very large
  number of rows.

## Sub-band search

A burst that is bright in only part of the band is diluted when the whole
band is summed. `--subband-levels` also searches parts of the band:

| Level | Planes searched | Total |
|---|---|---|
| `0` (default) | the full band | 1 |
| `1` | the full band, both halves, and the half straddling the middle | 4 |
| `2` | also the four quarters and the three quarters straddling their boundaries | 11 |

All planes use the same `--detect-thresh`. Searching more planes also means
more trials, and so more false detections from noise at a given threshold;
frost does not yet correct for this.

A burst detected in several planes is merged into one row (see
[Candidates](candidates.md)), which records both the widest band that
detected it and the band where it was strongest.

```bash
frost --file obs.fil --dm-max 2000 --subband-levels 2
```

## Rate limit

`--max-giant-rate R` caps the detections kept from each plane at `R` per
minute of data. Once a gulp exceeds the cap in a plane, frost drops that
plane's remaining detections for the gulp and logs a warning. Detections
are dropped in engine order, not by S/N, so this is a guard against RFI
storms, not a way to choose the best candidates. It is off by default.

## Capacities

Two limits bound the memory used for detections:

- `--max-candidates` (default 131072) per engine call, over all planes;
- `--max-cands-per-dm` (default 4096) per plane and DM trial.

When a gulp produces more, the extra detections are dropped and frost warns
once with the counts. Persistent overflow usually means unexcised RFI or a
threshold that is too low; see [RFI excision](rfi.md).
