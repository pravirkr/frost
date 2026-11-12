# Memory and performance

## Gulps and memory

frost processes data in gulps of `--nsamps-gulp` samples (default 262144).
Longer gulps search more efficiently, but memory grows with the gulp, the
number of channels and the number of DM trials.

Before searching, frost estimates the memory it needs and compares it with
what is available:

- With the default gulp, if the estimate does not fit, frost shortens the
  gulp until it does and logs a warning with both sizes.
- A gulp you set with `--nsamps-gulp` is never shortened: if it does not
  fit, frost stops with an error before allocating anything.

The gulp is always kept longer than the largest dispersion delay plus the
widest filter. `-v` logs the estimate by component (input, RFI, FDMT and
search, post-processing, reserve).

`--memory-mib N` sets the budget in MiB instead of using what is available.
It applies to the backend that runs the search: host RAM on a CPU run,
device memory on a GPU run.

## Speed

- frost uses OpenMP threads; set `OMP_NUM_THREADS` to limit them.
- `--async-io` (on by default) reads the next gulp while the current one is
  searched. Turn it off with `--no-async-io` to save one gulp of input
  memory.
- A higher `--dm-tol`, lower `--dm-max`, lower `--boxcar-max` or fewer
  sub-band levels all reduce work.
- A build tuned for the local CPU (the default) is faster than a portable
  one; see [Installation](../getting_started/installation.md).

## Profiles

`--profile` prints, at the end of the run, a table of the time spent in
each stage (read, RFI and its passes, FDMT and matched filtering,
clustering, sub-band merging, writing) with each stage's speed relative to
real time. Below it come the samples read and searched, the number of DM
trials, threads and candidates, memory capacities and any dropped
detections. `-vv` logs stage timings for every gulp.

## Provenance

`--provenance` writes `run.toml` in the output directory. It is written
when the run starts and updated when it ends, and records:

- `status` (`started`, `complete` or `failed`) and any error message;
- the command line (`argv`);
- `[build]`: frost's version and revision, the dmt and psrio revisions,
  compiler and build settings, as in `frost --version`;
- `[input]`: channel count, bits per sample, sampling time, frequencies and
  start time (MJD) of the input;
- `[config_effective]`: every setting after the command line and memory
  planning were applied, in [configuration file](configuration.md) format;
- `[memory_plan]` and `[memory]`: the estimate and the measured
  capacities;
- `[run]`: gulps, samples read and searched, candidates, dropped
  detections, threads and wall time.

Keep `run.toml` with the candidates: it is enough to repeat the search.

## Benchmark

```bash
frost --benchmark
```

runs a fixed synthetic observation (512 channels, 8-bit, 64 µs samples,
33 s, with one injected pulse at DM 100) through the whole pipeline and
prints its profile. The search settings are fixed, so results from
different machines and builds can be compared. `--device`, `--device-id`,
`--no-async-io` and `-v` still apply.
