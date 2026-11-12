# /// script
# requires-python = ">=3.10"
# dependencies = ["fonttools", "resvg-py", "pillow"]
# ///
"""Generate the frost logo.

The mark is a six-armed snowflake whose arms are FDMT merge trees: each arm
is a trunk with pairs of branches merging into it, separated from the central
node by a crisp negative-space collar. The lockup sets "frost" in Sora
SemiBold beside the mark, optionally above the tracked uppercase tagline
"RADIO TRANSIENT SEARCH" in IBM Plex Sans. Text is converted to outlines, so
the SVGs render identically without the fonts installed.

Usage::

    uv run docs/logo-src/make_logo.py            # SVGs used by README + docs
    uv run docs/logo-src/make_logo.py --exports  # also PNG/ICO/mono/social exports

The first form rewrites ``docs/_static/logo/``. Exports go to
``docs/logo-src/build/`` (git-ignored).
"""

from __future__ import annotations

import argparse
import hashlib
import io
import math
import urllib.request
from pathlib import Path

import resvg_py
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont
from PIL import Image

HERE = Path(__file__).resolve().parent
STATIC = HERE.parent / "_static" / "logo"
BUILD = HERE / "build"
CACHE = HERE / ".cache"

# Fonts, all SIL Open Font License 1.1. Pinned by version and hash.
FONTS = {
    "sora": (
        (
            "https://cdn.jsdelivr.net/npm/@fontsource/sora@5.2.8"
            "/files/sora-latin-600-normal.woff"
        ),
        "0f54f64e7e937c760a41c40d4a8402eb3f5d93021b603fd1bab937f86dc53c42",
    ),
    "plex-sans": (
        (
            "https://cdn.jsdelivr.net/npm/@ibm/plex-sans@1.1.0"
            "/fonts/complete/woff/IBMPlexSans-Regular.woff"
        ),
        "b731cf56514a4bd711ab2f9acf641f9311707f4386772eb306d25c2b29b73b1a",
    ),
}

# Palette.
NIGHT = "#08121F"  # polar night: dark ground and ink on light
ICE = "#8FD3FF"  # accent: the mark on dark
GLACIER = "#0B5A8A"  # the mark on light
FROST = "#EAF6FF"  # pale tint
WHITE = "#FFFFFF"
SUB_LIGHT = "#4E637A"  # tagline on light
SUB_DARK = "#8FA3BC"  # tagline on dark

NAME = "frost"
TAGLINE = "RADIO TRANSIENT SEARCH"

# Mark geometry: a 120-unit box centred on (60, 60). Each detail level lists
# one arm (trunk first) as line segments, with its stroke width and the
# radius of the centre dot (0 for none). Starting the trunk outside (60, 60)
# leaves an open negative-space collar around the central node.
_SIN56 = math.sin(math.radians(56.0))
_COS56 = math.cos(math.radians(56.0))

DETAIL = {
    "full": (
        [
            ((60.0, 48.0), (60.0, 11.5)),
            ((60.0, 39.2), (60.0 - 14.8 * _SIN56, 39.2 - 14.8 * _COS56)),
            ((60.0, 39.2), (60.0 + 14.8 * _SIN56, 39.2 - 14.8 * _COS56)),
            ((60.0, 24.5), (60.0 - 9.6 * _SIN56, 24.5 - 9.6 * _COS56)),
            ((60.0, 24.5), (60.0 + 9.6 * _SIN56, 24.5 - 9.6 * _COS56)),
        ],
        5.4,
        5.6,
    ),
    "icon": (
        [
            ((60.0, 46.5), (60.0, 12.0)),
            ((60.0, 35.0), (60.0 - 16.5 * _SIN56, 35.0 - 16.5 * _COS56)),
            ((60.0, 35.0), (60.0 + 16.5 * _SIN56, 35.0 - 16.5 * _COS56)),
        ],
        6.6,
        6.2,
    ),
    "small": (
        [
            ((60.0, 60.0), (60.0, 14.0)),
            ((60.0, 36.0), (46.0, 24.0)),
            ((60.0, 36.0), (74.0, 24.0)),
        ],
        11.5,
        0.0,
    ),
}
MARK_HALF = 51.2  # drawn half-extent of the "full" mark from centre (60 - 11.5 + 2.7)


def fmt(x: float) -> str:
    s = f"{x:.2f}".rstrip("0").rstrip(".")
    return "0" if s == "-0" else s


# ---------------------------------------------------------------- mark
def rotate(p: tuple[float, float], deg: float) -> tuple[float, float]:
    a = math.radians(deg)
    x, y = p[0] - 60, p[1] - 60
    return (
        60 + x * math.cos(a) - y * math.sin(a),
        60 + x * math.sin(a) + y * math.cos(a),
    )


def mark_body(stroke: str, dot: str | None, detail: str = "full") -> str:
    """Six merge-tree arms around a centre dot, in the 120-unit box."""
    segments, width, radius = DETAIL[detail]
    d = []
    for k in range(6):
        for a, b in segments:
            (x0, y0), (x1, y1) = rotate(a, 60 * k), rotate(b, 60 * k)
            d.append(f"M{fmt(x0)},{fmt(y0)}L{fmt(x1)},{fmt(y1)}")
    body = (
        f'<path d="{"".join(d)}" fill="none" stroke="{stroke}" '
        f'stroke-width="{fmt(width)}" stroke-linecap="round"/>'
    )
    if dot and radius:
        body += f'<circle cx="60" cy="60" r="{fmt(radius)}" fill="{dot}"/>'
    return body


# ---------------------------------------------------------------- type
def font_path(key: str) -> Path:
    url, sha = FONTS[key]
    path = CACHE / url.rsplit("/", 1)[1]
    if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != sha:
        CACHE.mkdir(parents=True, exist_ok=True)
        # The URLs are fixed https constants, and the payload is hash-checked.
        data = urllib.request.urlopen(url, timeout=30).read()  # noqa: S310
        digest = hashlib.sha256(data).hexdigest()
        if digest != sha:
            msg = f"{path.name}: unexpected sha256 {digest}"
            raise SystemExit(msg)
        path.write_bytes(data)
    return path


class Face:
    def __init__(self, key: str) -> None:
        self.font = TTFont(font_path(key))
        self.glyphs = self.font.getGlyphSet()
        self.cmap = self.font.getBestCmap()
        self.upm = self.font["head"].unitsPerEm
        self.hmtx = self.font["hmtx"]

    def ink_bounds(
        self, text: str, size: float, tracking: float = 0.0
    ) -> tuple[float, ...]:
        """Ink box (x0, y0, x1, y1) of ``text`` set at origin, y down."""
        _, bounds = self._set(text, size, 0, 0, tracking)
        return bounds

    def outline(
        self, text: str, size: float, x0: float, baseline: float, tracking: float = 0.0
    ) -> str:
        """Path data for ``text``; ``tracking`` is in em."""
        d, _ = self._set(text, size, x0, baseline, tracking)
        return d

    def _set(
        self, text: str, size: float, x0: float, baseline: float, tracking: float
    ) -> tuple[str, tuple[float, ...]]:
        scale = size / self.upm
        x, parts = x0, []
        bounds = BoundsPen(self.glyphs)
        for i, ch in enumerate(text):
            name = self.cmap[ord(ch)]
            transform = (scale, 0, 0, -scale, x, baseline)
            pen = SVGPathPen(self.glyphs, ntos=fmt)
            self.glyphs[name].draw(TransformPen(pen, transform))
            self.glyphs[name].draw(TransformPen(bounds, transform))
            parts.append(pen.getCommands())
            x += self.hmtx[name][0] * scale
            if i < len(text) - 1:
                x += tracking * size
        return "".join(parts), bounds.bounds


# ---------------------------------------------------------------- compositions
def svg(
    viewbox: tuple[float, ...],
    body: str,
    label: str,
    size: tuple[float, float] | None = None,
) -> str:
    vb = " ".join(fmt(v) for v in viewbox)
    dims = f'width="{fmt(size[0])}" height="{fmt(size[1])}" ' if size else ""
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" {dims}viewBox="{vb}" role="img" '
        f'aria-label="{label}"><title>{label}</title>{body}</svg>\n'
    )


def mark_svg(
    stroke: str, dot: str | None, detail: str = "full", pad: float = 2.8
) -> str:
    lo = 60 - MARK_HALF - pad
    span = 2 * (MARK_HALF + pad)
    return svg((lo, lo, span, span), mark_body(stroke, dot, detail), NAME)


def placed_mark(
    x: float,
    y: float,
    size: float,
    stroke: str,
    dot: str | None,
    detail: str = "full",
) -> str:
    """The 120-unit mark box drawn at (x, y), ``size`` px square."""
    k = size / 120
    return (
        f'<g transform="translate({fmt(x)} {fmt(y)}) scale({k:.5f})">'
        f"{mark_body(stroke, dot, detail)}</g>"
    )


def lockup_svg(
    sora: Face,
    sans: Face,
    text: str,
    stroke: str,
    dot: str,
    sub: str,
    *,
    tagline: bool = False,
    background: str | None = None,
) -> str:
    """Mark beside the wordmark, optionally with the tracked uppercase tagline."""
    name_size = 104.0 if tagline else 96.0
    name_track = -0.025
    nx0, ny0, nx1, ny1 = sora.ink_bounds(NAME, name_size, name_track)
    name_w = nx1 - nx0
    name_h = ny1 - ny0

    if tagline:
        sub_sz = 14.5
        sub_track = 0.165
        sx0, sy0, sx1, sy1 = sans.ink_bounds(TAGLINE, sub_sz, sub_track)
        sub_sz *= (name_w - 2.0) / (sx1 - sx0)
        sx0, sy0, sx1, sy1 = sans.ink_bounds(TAGLINE, sub_sz, sub_track)
        sub_h = sy1 - sy0
        gap_y = 14.0
        block_h = name_h + gap_y + sub_h
        drawn_mark_h = block_h + 4.0
        gap_x = 24.0
    else:
        sx0 = sy0 = sub_sz = sub_track = gap_y = 0.0
        block_h = name_h
        drawn_mark_h = name_h * 1.32
        gap_x = 22.0

    drawn_frac = (2 * MARK_HALF) / 120.0
    mark_box = drawn_mark_h / drawn_frac
    mark_pad = (60.0 - MARK_HALF) / 120.0 * mark_box

    pad_x = 36.0 if background else 5.0
    pad_y = 28.0 if background else 5.0
    w = 2 * pad_x + drawn_mark_h + gap_x + name_w
    h = 2 * pad_y + max(drawn_mark_h, block_h)
    cy = h / 2.0

    parts = []
    if background:
        parts.append(f'<rect width="{fmt(w)}" height="{fmt(h)}" fill="{background}"/>')
    parts.append(
        placed_mark(pad_x - mark_pad, cy - mark_box / 2.0, mark_box, stroke, dot)
    )

    tx = pad_x + drawn_mark_h + gap_x
    top_y = cy - block_h / 2.0
    parts.append(
        f'<path d="{sora.outline(NAME, name_size, tx - nx0, top_y - ny0, name_track)}" '
        f'fill="{text}"/>'
    )
    if tagline:
        parts.append(
            f'<path d="{sans.outline(TAGLINE, sub_sz, tx - sx0 + 1.0, top_y + name_h + gap_y - sy0, sub_track)}" '
            f'fill="{sub}"/>'
        )
    label = f"{NAME}: Radio Transient Search" if tagline else NAME
    return svg((0, 0, w, h), "".join(parts), label)


def favicon_svg() -> str:
    """Small-size mark that follows the browser's light/dark preference."""
    body = mark_body("CLR", None, "small").replace('stroke="CLR"', 'class="m"')
    body = (
        f"<style>.m{{stroke:{GLACIER}}}@media (prefers-color-scheme:dark)"
        f"{{.m{{stroke:{ICE}}}}}</style>{body}"
    )
    return svg((0, 0, 120, 120), body, NAME)


def tile_svg(bg: str, stroke: str, dot: str, radius: float = 28 / 128) -> str:
    """Rounded app icon / avatar tile with the simplified mark."""
    s = 512
    inner = s * 88 / 128
    body = f'<rect width="{s}" height="{s}" rx="{fmt(s * radius)}" fill="{bg}"/>'
    body += placed_mark((s - inner) / 2, (s - inner) / 2, inner, stroke, dot, "icon")
    return svg((0, 0, s, s), body, NAME)


def social_svg(lockup_dark: str) -> str:
    """1280x640 GitHub social preview: tagline lockup centred on polar night."""
    width = 760.0
    height = width / aspect(lockup_dark)
    nested = lockup_dark.replace(
        "<svg ",
        f'<svg x="{fmt((1280 - width) / 2)}" y="{fmt((640 - height) / 2)}" '
        f'width="{fmt(width)}" height="{fmt(height)}" ',
        1,
    )
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="640" '
        f'viewBox="0 0 1280 640"><rect width="1280" height="640" fill="{NIGHT}"/>'
        f"{nested}</svg>\n"
    )


# ---------------------------------------------------------------- output
def write(path: Path, content: str | bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(content, str):
        path.write_text(content)
    else:
        path.write_bytes(content)
    print(f"  {path.relative_to(HERE.parent.parent)}")


def png(svg_text: str, width: int, height: int) -> bytes:
    return bytes(resvg_py.svg_to_bytes(svg_string=svg_text, width=width, height=height))


def aspect(svg_text: str) -> float:
    vb = [float(v) for v in svg_text.split('viewBox="', 1)[1].split('"', 1)[0].split()]
    return vb[2] / vb[3]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument(
        "--exports",
        action="store_true",
        help="also write PNG/ICO/mono/social exports to build/",
    )
    args = parser.parse_args()

    sora, sans = Face("sora"), Face("plex-sans")
    tagline_light = lockup_svg(sora, sans, NIGHT, GLACIER, ICE, SUB_LIGHT, tagline=True)
    tagline_dark = lockup_svg(sora, sans, WHITE, ICE, WHITE, SUB_DARK, tagline=True)

    # Files referenced by README.md, docs/conf.py and docs/index.md.
    print("docs/_static/logo:")
    write(STATIC / "frost-logo-tagline.svg", tagline_light)
    write(STATIC / "frost-logo-tagline-dark.svg", tagline_dark)
    write(
        STATIC / "frost-logo.svg",
        lockup_svg(sora, sans, NIGHT, GLACIER, ICE, SUB_LIGHT),
    )
    write(
        STATIC / "frost-logo-dark.svg",
        lockup_svg(sora, sans, WHITE, ICE, WHITE, SUB_DARK),
    )
    write(STATIC / "frost-mark.svg", mark_svg(GLACIER, ICE))
    write(STATIC / "frost-mark-dark.svg", mark_svg(ICE, WHITE))
    write(STATIC / "favicon.svg", favicon_svg())

    if not args.exports:
        return

    print("docs/logo-src/build:")
    assets = {
        "frost-logo-tagline": (
            lockup_svg(
                sora,
                sans,
                NIGHT,
                GLACIER,
                ICE,
                SUB_LIGHT,
                tagline=True,
                background=WHITE,
            ),
            2400,
        ),
        "frost-logo-tagline-dark": (
            lockup_svg(
                sora,
                sans,
                WHITE,
                ICE,
                WHITE,
                SUB_DARK,
                tagline=True,
                background=NIGHT,
            ),
            2400,
        ),
        "frost-logo": (
            lockup_svg(sora, sans, NIGHT, GLACIER, ICE, SUB_LIGHT),
            2400,
        ),
        "frost-logo-dark": (
            lockup_svg(sora, sans, WHITE, ICE, WHITE, SUB_DARK),
            2400,
        ),
        "frost-mark": (mark_svg(GLACIER, ICE), 1024),
        "frost-mark-dark": (mark_svg(ICE, WHITE), 1024),
        # One colour, for print and stickers (collar keeps the centre dot distinct).
        "frost-mark-mono": (mark_svg(NIGHT, NIGHT), 1024),
        "frost-mark-mono-dark": (mark_svg(WHITE, WHITE), 1024),
        # App icon / avatar tiles (GitHub organisation, conda-forge).
        "frost-icon-dark": (tile_svg(NIGHT, ICE, WHITE), 1024),
        "frost-icon-ice": (tile_svg(ICE, NIGHT, NIGHT), 1024),
    }
    for name, (text, width) in assets.items():
        write(BUILD / f"{name}.svg", text)
        write(BUILD / f"{name}.png", png(text, width, round(width / aspect(text))))

    # Favicon and touch icon: the small-size mark on the dark tile.
    small = svg(
        (0, 0, 512, 512),
        f'<rect width="512" height="512" rx="102" fill="{NIGHT}"/>'
        + placed_mark(36, 36, 440, ICE, None, "small"),
        NAME,
    )
    write(BUILD / "apple-touch-icon.png", png(tile_svg(NIGHT, ICE, WHITE), 180, 180))
    frames = [
        Image.open(io.BytesIO(png(small, s, s))).convert("RGBA")
        for s in (64, 48, 32, 16)
    ]
    buf = io.BytesIO()
    frames[0].save(
        buf,
        format="ICO",
        sizes=[(s, s) for s in (16, 32, 48, 64)],
        append_images=frames[1:],
    )
    write(BUILD / "favicon.ico", buf.getvalue())
    write(BUILD / "social-preview.png", png(social_svg(tagline_dark), 1280, 640))


if __name__ == "__main__":
    main()
