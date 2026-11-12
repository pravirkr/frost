# frost logo source

The mark is a six-armed snowflake whose arms are FDMT merge trees: a trunk
with pairs of branches merging into it, as the transform merges sub-band
partial sums. The lockup sets "frost" in Sora SemiBold beside it. The README
banner sets the lockup and the name's expansion beside a dispersed sweep
across the channels frozen into a straight pulse.

The design is the Frost identity sheet from the branding canvas; this
script reproduces it.

## Regenerate

```bash
uv run docs/logo-src/make_logo.py            # rewrites docs/_static/logo/*.svg
uv run docs/logo-src/make_logo.py --exports  # also writes build/ (PNG, ICO, mono, social)
```

The script downloads its fonts once into `.cache/` and checks their sha256:
Sora SemiBold (`@fontsource/sora` 5.2.8) and IBM Plex Mono Regular
(`@ibm/plex-mono` 1.1.0), both under the SIL Open Font License 1.1.
Text is converted to outlines, so the SVGs need no fonts to render. Building
or using frost never needs Python; only this script and the docs do.

## Files in the repo

Only the files the README and docs reference are committed:

| File | Used by |
| --- | --- |
| `frost-banner.svg`, `frost-banner-dark.svg` | `README.md` header, picked by GitHub's light/dark theme; transparent background |
| `frost-logo.svg`, `frost-logo-dark.svg` | Sphinx sidebar (`docs/conf.py`) |
| `frost-mark.svg`, `frost-mark-dark.svg` | Docs landing page hero (`docs/index.md`) |
| `favicon.svg` | Sphinx favicon (follows the browser's light/dark preference) |

Everything else comes from `--exports`: high-resolution PNGs (the dark
banner on a solid polar-night ground), one-colour
marks for print, rounded app-icon tiles (dark and ice, for a GitHub avatar),
`favicon.ico`, `apple-touch-icon.png`, and `social-preview.png` (1280×640,
for GitHub Settings → Social preview).

## Parameters

| Constant | Value | Meaning |
| --- | --- | --- |
| `NIGHT` | `#08121F` | Polar night: dark ground, and ink on light |
| `ICE` | `#8FD3FF` | The mark on dark; the centre dot on light |
| `GLACIER` | `#0B5A8A` | The mark on light; docs link colour |
| `FROST` | `#EAF6FF` | Pale tint (docs hero background) |
| `DETAIL` | `full`, `icon`, `small` | Arm geometry per size: two branch pairs and stroke 6 for large marks, one pair for icons, one pair and stroke 12 without the dot for favicons |

The mark lives in a 120-unit box centred on (60, 60); each arm is listed
once and repeated every 60°.
