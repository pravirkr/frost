# User guide

frost is one command, `frost`. It reads one input, searches it and writes
candidates. Every option has a default, so a search needs only an input and
usually a DM range:

```bash
frost --file obs.fil --dm-max 2000
```

Each gulp of data passes through the same stages:

1. **Read** a gulp of spectra from a [filterbank file or PSRDADA ring](inputs.md).
2. **Clean** it: [RFI excision](rfi.md), then calibrate every channel to
   zero mean and unit noise.
3. **Search** it: dedisperse every DM trial with FDMT, matched-filter each
   trial with boxcars of increasing width, and find peaks above threshold,
   in the full band and optionally in [sub-bands](search.md).
4. **Reduce** the detections: cluster each event's detections into one row
   and merge sub-band detections of the same event.
5. **Write** the rows to a [`.cand` file](candidates.md).

Settings come from three places, later ones winning:

1. frost's built-in defaults;
2. a TOML [configuration file](configuration.md) given with `--config`;
3. options on the command line.

The pages in this guide:

- [Inputs](inputs.md): file formats, PSRDADA, beams and channel order.
- [Configuration files](configuration.md): the TOML format and every key.
- [RFI excision](rfi.md): what is on by default, and the other strategies.
- [The search](search.md): DM trials, widths, thresholds and sub-bands.
- [Candidates](candidates.md): clustering and the `.cand` format.
- [GPU runs](gpu.md): building and running on CUDA or HIP.
- [Memory and performance](resources.md): gulps, memory limits, profiles,
  provenance and the benchmark.
- [Quirks and gotchas](quirks.md): things that are easy to trip over.

The [CLI reference](../reference/cli.md) lists every option.
