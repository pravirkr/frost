---
sd_hide_title: true
---

# frost

```{raw} html
<div class="frost-hero">
  <div class="frost-hero-copy">
    <p class="frost-hero-title">FDMT-accelerated Optimal Search for Radio Transients</p>
    <p class="frost-hero-lede">
      One command searches channelised intensity data for fast radio bursts
      and single pulses: RFI excision, dedispersion with the Fast Dispersion
      Measure Transform, matched filtering, and a clean list of candidates.
    </p>
    <div class="frost-badges">
      <span class="frost-badge">Filterbank · HDF5 · PSRDADA</span>
      <span class="frost-badge">Sub-band search</span>
      <span class="frost-badge">C++20 · OpenMP</span>
      <span class="frost-badge">CUDA &amp; HIP</span>
    </div>
  </div>
  <img class="frost-hero-watermark only-light" src="_static/logo/frost-mark.svg" alt="" aria-hidden="true">
  <img class="frost-hero-watermark only-dark" src="_static/logo/frost-mark-dark.svg" alt="" aria-hidden="true">
</div>
```

```bash
frost --file obs.fil --dm-max 1000 --output-dir out
```

frost cleans each gulp, searches every DM trial up to 1000 pc cm⁻³ and every
pulse width up to 4096 samples, clusters detections in time, DM, width and
sub-band, and writes one row per candidate to `out/candidates.cand`.

## Features

::::{grid} 1 2 2 2
:gutter: 3

:::{grid-item-card} Inputs
:class-card: sd-shadow-sm

- Sigproc filterbank (`.fil`).
- Breakthrough Listen HDF5 filterbank, one beam of a multi-beam file.
- Live PSRDADA ring buffers (Linux).
:::

:::{grid-item-card} RFI excision
:class-card: sd-shadow-sm

- Narrow- and broad-band excision on by default.
- Opt-in channel moments, IQRM, Kadane runs, dropouts, zero-DM and zdot.
- Static zaps by channel or frequency, and sigproc kill files.
:::

:::{grid-item-card} Search
:class-card: sd-shadow-sm

- Streaming FDMT (Zackay & Ofek 2017) through [dmt](https://github.com/pravirkr/dmt).
- Boxcar or trapezoid matched filters with an exact noise model.
- The full band, or also its halves and quarters, for band-limited bursts.
:::

:::{grid-item-card} Candidates
:class-card: sd-shadow-sm

- Friends-of-friends clustering in time, DM and width.
- Nested sub-band detections merged along the dispersion curve.
- A tab-separated `.cand` file, and an optional `run.toml` record.
:::

::::

## Where to go next

::::{grid} 1 2 2 2
:gutter: 3

:::{grid-item-card} Installation
:link: getting_started/installation
:link-type: doc
:class-card: sd-shadow-sm

Build frost from source and install the `frost` command.
:::

:::{grid-item-card} First search
:link: getting_started/quickstart
:link-type: doc
:class-card: sd-shadow-sm

Search a filterbank file and read the candidates it writes.
:::

:::{grid-item-card} User guide
:link: user_guide/index
:link-type: doc
:class-card: sd-shadow-sm

Inputs, configuration files, RFI, the search, candidates, GPUs and memory.
:::

:::{grid-item-card} CLI reference
:link: reference/cli
:link-type: doc
:class-card: sd-shadow-sm

Every `frost` option, its default and its configuration key.
:::

::::

```{toctree}
:hidden:
:caption: Getting started

getting_started/installation
getting_started/quickstart
```

```{toctree}
:hidden:
:caption: User guide

user_guide/index
user_guide/inputs
user_guide/configuration
user_guide/rfi
user_guide/search
user_guide/candidates
user_guide/gpu
user_guide/resources
user_guide/quirks
```

```{toctree}
:hidden:
:caption: Reference

reference/cli
reference/library
changelog
citing
```
