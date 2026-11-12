# First search

This page runs one search on a filterbank file and explains what frost
writes. It assumes `frost` is [installed](installation.md) and on your
`PATH`.

## Run the search

```bash
frost --file obs.fil --dm-max 1000 --output-dir out -v
```

| Option | Meaning |
|---|---|
| `--file obs.fil` | The input: a sigproc filterbank, or a Breakthrough Listen HDF5 file (`.h5`). |
| `--dm-max 1000` | Search DMs from 0 to 1000 pc cm⁻³. |
| `--output-dir out` | Write candidates to `out/`, creating it if needed. |
| `-v` | Log memory planning and one line per gulp. |

frost reads the file in *gulps* of up to 262144 samples. For each gulp it
excises RFI, calibrates every channel to zero mean and unit noise,
dedisperses every DM trial with FDMT, convolves each trial with boxcars of
1, 2, 4, … 4096 samples, and keeps peaks above 6σ. Detections of one event
at neighbouring times, DMs and widths are then clustered into one row. At
the end it prints a summary line:

```text
frost: read <N> sample(s) in <G> gulp(s) and searched <M> from obs.fil
```

`searched` can be smaller than `read`: the last samples of the input, shorter
than the largest dispersion delay, cannot be searched at every DM.

If the default gulp does not fit in memory, frost picks a shorter one and
says so in a warning. See [Memory and performance](../user_guide/resources.md).

## Read the candidates

`out/candidates.cand` is a tab-separated text file with no header, one row
per candidate. The first nine columns follow the common single-pulse
layout:

| # | Column | Meaning |
|---|---|---|
| 1 | `snr` | Detection S/N |
| 2 | `samp_idx` | Sample index of the detection, counted from the start of the input |
| 3 | `time` | `samp_idx` in seconds |
| 4 | `filter_idx` | Width-trial index (width = 2^idx samples with default settings) |
| 5 | `dm_idx` | DM-trial index |
| 6 | `dm` | DM (pc cm⁻³) |
| 7 | `members` | Number of detections merged into this row |
| 8, 9 | `begin`, `end` | First and last above-threshold sample |

Fourteen more columns describe the frequency band of the detection; see
[Candidates](../user_guide/candidates.md).

To list the ten brightest candidates:

```bash
sort -t $'\t' -k1,1gr out/candidates.cand | head
```

To keep only S/N ≥ 8 and DM ≥ 3:

```bash
awk -F '\t' '$1 >= 8 && $6 >= 3' out/candidates.cand
```

## Next steps

- Use a [configuration file](../user_guide/configuration.md) to keep the
  settings of a survey in one place.
- Turn on more [RFI strategies](../user_guide/rfi.md) for dirty data.
- Search [sub-bands](../user_guide/search.md) for band-limited bursts.
- Record each run with `--provenance` ([Memory and
  performance](../user_guide/resources.md)).
