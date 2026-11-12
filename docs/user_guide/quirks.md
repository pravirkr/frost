# Quirks and gotchas

## `-march=native` binaries do not travel

A default Release build is tuned for the CPU it was built on. Copied to an
older or different CPU, `frost` can stop with "illegal instruction". Build
with `-DFROST_ENABLE_NATIVE_ARCH=OFF`, or set `FROST_CPU_ARCH`, for a
binary that runs on several machines.

## The gulp may be shorter than you think

If the default 262144-sample gulp does not fit in memory, frost shortens it
and warns. Set `--nsamps-gulp` to fix the gulp; frost then fails clearly
instead of shortening it. See [Memory and performance](resources.md).

## Output files are overwritten

A second run with the same `--output-dir` replaces `candidates.cand`, and
per-gulp files of the same input are named the same each time. Use a
separate output directory per run.

## Zero-DM filtering removes DM 0

`--rfi-zero-dm subtract` and `zdot` remove every signal at DM 0, so
candidates near DM 0 mean nothing after them, and wide pulses at low DM
lose S/N. See [RFI excision](rfi.md#transforms).

## Channel numbers follow `--fswap`

`--zap-chans` and the `chan_begin`/`chan_end` columns count channels in the
order frost reads them, which `--fswap` reverses. Kill files are mirrored
automatically; `--zap-freqs` is unaffected.

## More sub-bands, more false positives

`--subband-levels 2` searches 11 planes at one threshold, so pure noise
crosses it more often than in a full-band search. Expect more weak
candidates, and raise `--detect-thresh` if they are a problem.

## `--max-giant-rate` drops by order, not by S/N

The rate limit keeps the first detections of a gulp in engine order and
drops the rest, whatever their S/N. Use it as a guard against RFI storms.

## The `.cand` file has no absolute time

`time` counts from the start of the input. Use `--provenance`; `run.toml`
records the start time.

## NaN or infinity stops the run

Float input with a non-finite sample is rejected with an error. frost does
not repair it; fix or mask the data first.
