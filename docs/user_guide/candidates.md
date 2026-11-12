# Candidates

## From detections to rows

One pulse is detected at many neighbouring times, DM trials and widths,
and in several sub-band planes. Before writing, frost reduces these to one
row per event, in two passes:

1. **Clustering within each plane.** Detections are grouped
   friends-of-friends: two belong together when they are within
   `--cand-sep-time` samples (default 3), `--cand-sep-filter` width trials
   (default 3) and `--cand-sep-dm` DM trials (default 200) of each other.
   Beyond adjacent DM trials, they must also be within
   `--cand-sep-dm-smear` (default 8) boxcar widths of dispersive smearing in
   DM. This physical tolerance keeps a fine DM grid from joining unrelated
   events. A second pass lets a bright cluster absorb weaker clusters on its
   DM wings. Each cluster is written as its brightest detection, with
   `members` counting the detections it holds.
2. **Merging sub-bands.** With `--subband-levels` 1 or 2, rows from
   different planes are merged when their bands nest and they line up on
   one dispersion curve in time, DM and width. The row keeps the widest
   band's coordinates and adds those of the strongest member.

`--subband-dedup` chooses how sub-band matches form groups:

- `transitive` (default): a narrow-band row that matches two wide-band rows
  joins them into one. This keeps the row count low, but a weak event close
  to a bright one in time and DM can be absorbed.
- `seeded`: rows are placed widest and brightest first, and each joins the
  first group it matches. Groups never merge, so distinct events stay
  apart, at the cost of more rows from the DM wings of bright pulses.

`--no-cluster` skips both passes and writes every detection.

### Events across gulp boundaries

A pulse near the end of a gulp may continue into the next one. frost holds
unfinished clusters until no later detection can join them, then writes
them. `--max-pending-candidates` bounds how many detections are held
(default 0, meaning the same as `--max-candidates`); exceeding it stops the
run with an error rather than dropping candidates.

## Output files

By default frost writes all candidates of a run to
`<output-dir>/candidates.cand`. With `--per-gulp-files` it writes one file
per gulp, named after the gulp's start time (UTC) and the beam number, for
example `2026-11-12-13:53:51_01.cand`. This suits live searches, where
candidates should reach disk while the stream runs.

An existing file of the same name is overwritten.

## The `.cand` format

Each row is one candidate, with 23 tab-separated columns and no header.
The first nine follow the common single-pulse layout; the rest describe the
band of the detection and its strongest sub-band member.

| # | Column | Meaning |
|---|---|---|
| 1 | `snr` | Detection S/N |
| 2 | `samp_idx` | Sample index of the detection, counted from the start of the input |
| 3 | `time` | `samp_idx * tsamp`, seconds |
| 4 | `filter_idx` | Width-trial index |
| 5 | `dm_idx` | DM-trial index |
| 6 | `dm` | DM (pc cm⁻³) |
| 7 | `members` | Number of detections merged into this row |
| 8, 9 | `begin`, `end` | First and last above-threshold sample |
| 10, 11 | `f_low`, `f_high` | Frequency edges (MHz) of the band searched |
| 12, 13 | `chan_begin`, `chan_end` | Inclusive channel extent of that band, in the order frost reads channels (after `--fswap`) |
| 14 | `snr_best` | S/N of the strongest associated sub-band row |
| 15, 16 | `chan_begin_best`, `chan_end_best` | Channel extent of that row |
| 17 | `n_extents` | Number of distinct channel extents associated |
| 18 | `samp_idx_best` | Strongest member's sample index |
| 19 | `time_best` | Strongest member's time (s) |
| 20 | `dm_best` | Strongest member's DM |
| 21 | `width_best` | Strongest member's width (samples) |
| 22, 23 | `f_low_best`, `f_high_best` | Strongest member's frequency edges (MHz) |

Without sub-band merging, columns 14–23 repeat the row's own values and
`n_extents` is 1. Columns 18–23 give the strongest member's position in its
own frequency plane. For a narrow-band burst they can differ from the
wide-band row in columns 1–13, and are usually the better estimate of its
time and DM.

### Times

`samp_idx` counts samples from the start of the input. It marks the end of
the matched filter's window, at the pulse's arrival at the bottom edge of
the searched band. To convert it to an absolute time, add `time` to the
observation's start time, which `--provenance` records in `run.toml`
(`input.start_mjd`). The `.cand` file itself has no absolute time column.

## Reading the file

Any tool that reads tab-separated text works. In Python, for example:

```python
import pandas as pd

columns = [
    "snr", "samp_idx", "time", "filter_idx", "dm_idx", "dm", "members",
    "begin", "end", "f_low", "f_high", "chan_begin", "chan_end",
    "snr_best", "chan_begin_best", "chan_end_best", "n_extents",
    "samp_idx_best", "time_best", "dm_best", "width_best",
    "f_low_best", "f_high_best",
]
cands = pd.read_csv("out/candidates.cand", sep="\t", names=columns)
print(cands.sort_values("snr", ascending=False).head())
```
