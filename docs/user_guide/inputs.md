# Inputs

frost reads exactly one input per run: a file (`--file`) or a PSRDADA ring
buffer (`--dada-key`). Readers come from
[psrio](https://github.com/pravirkr/psrio).

## Filterbank files

```bash
frost --file obs.fil --dm-max 1000
frost --file obs.h5 --dm-max 1000
```

`--file` accepts:

- **sigproc filterbank** (`.fil`), with 1, 2, 4, 8 or 16-bit integer
  samples or 32-bit float samples;
- **Breakthrough Listen HDF5 filterbank** (`.h5`, `.hdf5`). frost
  recognises it by name or by content.

The sampling time, frequencies, start time and channel count come from the
file's header. Nothing about the observation is set on the command line.

### Multi-beam HDF5 files

An HDF5 filterbank can hold several beams (or IFs) in one cube. frost
searches one beam per run, chosen with the 0-based `--input-beam`
(default 0):

```bash
for beam in 0 1 2 3; do
    frost --file cube.h5 --input-beam "$beam" --beam "$((beam + 1))" \
          --dm-max 1000 --output-dir "out/beam$beam"
done
```

`--input-beam` is ignored for sigproc files and PSRDADA.

## PSRDADA ring buffers

On Linux builds with PSRDADA (the default there), frost can search a live
stream:

```bash
frost -k dada --dm-max 2000 --per-gulp-files --output-dir out
```

`-k`/`--dada-key` takes the ring's hexadecimal key, with or without `0x`.
frost reads the observation from the ring's DADA header and searches until
the writer ends the stream. `--per-gulp-files` writes one `.cand` file per
gulp, so candidates appear on disk while the stream runs (see
[Candidates](candidates.md)).

To search several beams, run one `frost -k` per ring.

## Float data

32-bit float samples must be finite. frost checks every sample before
searching and stops with an error naming the sample and channel if it finds
a NaN or infinity. It does not repair such data.

## Channel order and `--fswap`

frost searches positive DMs in the order the channels are stored, ascending
or descending frequency. `--fswap` reverses the channel order as frost reads
it, which searches the mirrored (negative) DM sweep.

Channel numbers in `--zap-chans` and `zap_chans` refer to channels as frost
sees them, after `--fswap`; kill files are mirrored to match, so one file
serves both orders. See [RFI excision](rfi.md#static-masks).

## Beam number in output names

`--beam` sets the 1-based beam number used in per-gulp output file names
(`<utc>_<beam>.cand`). The default, 0, takes the number from the input.
