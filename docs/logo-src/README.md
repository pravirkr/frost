# frost logo source

The mark is a six-armed snowflake whose arms are FDMT merge trees: each arm
is a trunk with pairs of branches merging into it, separated from the central
node by a negative-space collar. The lockup sets "frost" in Sora SemiBold
beside the mark, optionally above the tracked uppercase tagline
`RADIO TRANSIENT SEARCH` in IBM Plex Sans.

## Regenerate

```bash
uv run docs/logo-src/make_logo.py            # rewrites docs/_static/logo/*.svg
uv run docs/logo-src/make_logo.py --exports  # also writes build/ (PNG, ICO, mono, social)
```

The script downloads its fonts once into `.cache/` and checks their sha256:
Sora SemiBold (`@fontsource/sora` 5.2.8) and IBM Plex Sans Regular
(`@ibm/plex-sans` 1.1.0), both under the SIL Open Font License 1.1.
Text is converted to outlines, so the SVGs need no fonts to render. Building
or using frost never needs Python; only this script and the docs do.

## Files in the repo

Only the files the README and docs reference are committed:

| File | Used by |
| --- | --- |
| `frost-logo-tagline.svg`, `frost-logo-tagline-dark.svg` | `README.md` header, picked by GitHub's light/dark theme; transparent background |
| `frost-logo.svg`, `frost-logo-dark.svg` | Sphinx sidebar (`docs/conf.py`) |
| `frost-mark.svg`, `frost-mark-dark.svg` | Docs landing page hero (`docs/index.md`) |
| `favicon.svg` | Sphinx favicon (follows the browser's light/dark preference) |

Everything else comes from `--exports`: high-resolution PNGs, one-colour
marks for print, rounded app-icon tiles (dark and ice, for a GitHub avatar),
`favicon.ico`, `apple-touch-icon.png`, and `social-preview.png` (1280×640,
for GitHub Settings → Social preview).

## Parameters

| Constant | Value | Meaning |
| --- | --- | --- |
| `NIGHT` | `#08121F` | Polar night: dark ground, and ink on light |
| `ICE` | `#8FD3FF` | The mark on dark; the centre dot on light |
| `GLACIER` | `#0B5A8A` | The mark on light; docs link colour |
| `FROST` | `#EAF6FF` | Pale tint |
| `DETAIL` | `full`, `icon`, `small` | Arm geometry per size: two branch pairs with a negative-space centre collar at display size, one pair with collar for icons, one pair without dot at stroke 11.5 for favicons |

The mark lives in a 120-unit box centred on (60, 60); each arm is listed
once and repeated every 60°.
