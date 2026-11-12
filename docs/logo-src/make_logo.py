# /// script
# requires-python = ">=3.10"
# dependencies = ["fonttools", "resvg-py", "pillow"]
# ///
"""Generate the frost logo.

The mark is a six-armed snowflake whose arms are FDMT merge trees: each arm
is a trunk with pairs of branches merging into it, the way the transform
merges sub-band partial sums. The lockup sets "frost" in Sora SemiBold beside
it. The README banner, in light and dark versions, shows a dispersed sweep
frozen into a straight pulse.
Text is converted to outlines, so the SVGs render the same without the fonts
installed.

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
        "https://cdn.jsdelivr.net/npm/@fontsource/sora@5.2.8"
        "/files/sora-latin-600-normal.woff",
        "0f54f64e7e937c760a41c40d4a8402eb3f5d93021b603fd1bab937f86dc53c42",
    ),
    "plex-mono": (
        "https://cdn.jsdelivr.net/npm/@ibm/plex-mono@1.1.0"
        "/fonts/complete/woff/IBMPlexMono-Regular.woff",
        "1d5732f53287cbe58936deabda52a60d810b84ac4a3fbdefa8e7632a2c38c39d",
    ),
}

# Palette.
NIGHT = "#08121F"  # polar night: dark ground and ink on light
ICE = "#8FD3FF"  # accent: the mark on dark
GLACIER = "#0B5A8A"  # the mark on light
FROST = "#EAF6FF"  # pale tint
WHITE = "#FFFFFF"
GRID = "#142338"  # waterfall channels in the banner
MUTED = "#7E8DA6"  # banner labels

NAME = "frost"
EXPANSION = "FDMT-accelerated Optimal Search for Radio Transients"

# Mark geometry: a 120-unit box centred on (60, 60). Each detail level lists
# one arm (trunk first) as line segments, with its stroke width and the
# radius of the centre dot (0 for none). The arm is repeated every 60°.
# Fewer branches and heavier strokes keep the small sizes legible.
DETAIL = {
    "full": (
        [
            ((60, 60), (60, 12)),
            ((60, 40), (49, 29)),
            ((60, 40), (71, 29)),
            ((60, 25), (53, 18)),
            ((60, 25), (67, 18)),
        ],
        6.0,
        7.0,
    ),
    "icon": (
        [((60, 60), (60, 12)), ((60, 40), (49, 29)), ((60, 40), (71, 29))],
        7.0,
        8.0,
    ),
    "small": (
        [((60, 60), (60, 14)), ((60, 36), (46, 24)), ((60, 36), (74, 24))],
        12.0,
        0.0,
    ),
}
MARK_HALF = 54  # half-width of the cropped mark, units from the centre


def fmt(x: float) -> str:
    s = f"{x:.2f}".rstrip("0").rstrip(".")
    return "0" if s == "-0" else s


# ---------------------------------------------------------------- mark
def rotate(p: tuple[float, float], deg: float) -> tuple[float, float]:
    a = math.radians(deg)
    x, y = p[0] - 60, p[1] - 60
    return (60 + x * math.cos(a) - y * math.sin(a), 60 + x * math.sin(a) + y * math.cos(a))


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

    def ink_bounds(self, text: str, size: float, tracking: float = 0.0) -> tuple[float, ...]:
        """Ink box (x0, y0, x1, y1) of ``text`` set at origin, y down."""
        _, bounds = self._set(text, size, 0, 0, tracking)
        return bounds

    def outline(
        self, text: str, size: float, x0: float, baseline: float, tracking: float = 0.0
    ) -> str:
        """Path data for ``text``; ``tracking`` is in em."""
        d, _ = self._set(text, size, x0, baseline, tracking)
        return d

    def _set(self, text, size, x0, baseline, tracking):
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
def svg(viewbox, body: str, label: str, size: tuple[float, float] | None = None) -> str:
    vb = " ".join(fmt(v) for v in viewbox)
    dims = f'width="{fmt(size[0])}" height="{fmt(size[1])}" ' if size else ""
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" {dims}viewBox="{vb}" role="img" '
        f'aria-label="{label}"><title>{label}</title>{body}</svg>\n'
    )


def mark_svg(stroke: str, dot: str | None, detail: str = "full") -> str:
    lo, span = 60 - MARK_HALF, 2 * MARK_HALF
    return svg((lo, lo, span, span), mark_body(stroke, dot, detail), NAME)


def placed_mark(x: float, y: float, size: float, stroke: str, dot: str | None,
                detail: str = "full") -> str:
    """The 120-unit mark box drawn at (x, y), ``size`` px square."""
    k = size / 120
    return (
        f'<g transform="translate({fmt(x)} {fmt(y)}) scale({k:.5f})">'
        f"{mark_body(stroke, dot, detail)}</g>"
    )


def wordmark(sora: Face, size: float, x: float, centre_y: float, fill: str) -> tuple[str, float]:
    """"frost" with its ink box centred on ``centre_y``; returns body, right edge."""
    track = -0.03
    x0, y0, x1, y1 = sora.ink_bounds(NAME, size, track)
    baseline = centre_y - (y0 + y1) / 2
    d = sora.outline(NAME, size, x - x0, baseline, track)
    return f'<path d="{d}" fill="{fill}"/>', x + (x1 - x0)


def lockup_svg(sora: Face, text: str, stroke: str, dot: str) -> str:
    """Mark beside the wordmark, as on the design's primary lockups."""
    mark, gap, size = 112, 24, 96
    # Crop the mark box to the drawn arms so the lockup has no dead margin.
    pad = (120 - 2 * MARK_HALF) / 120 * mark
    body = placed_mark(0, 0, mark, stroke, dot)
    words, right = wordmark(sora, size, mark - pad + gap, mark / 2, text)
    body += words
    return svg((pad - 4, pad - 4, right - pad + 8, mark - 2 * pad + 8), body, NAME)


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


# Banner colours per GitHub theme. The background stays transparent, so the
# banner blends into the page; README.md picks one with <picture>.
BANNER = {
    "light": {"grid": "#DCE5EE", "sweep": GLACIER, "pulse": GLACIER, "arrow": "#8C9AB0",
              "mark": GLACIER, "dot": ICE, "name": NIGHT, "sub": GLACIER, "halo": GLACIER},
    "dark": {"grid": GRID, "sweep": ICE, "pulse": ICE, "arrow": MUTED,
             "mark": ICE, "dot": WHITE, "name": WHITE, "sub": ICE, "halo": None},
}


def banner_svg(sora: Face, mono: Face, theme: str, background: str | None = None) -> str:
    """1280x326 README header: the lockup and expansion beside a burst being
    dedispersed, its sweep across the channels frozen into a straight pulse."""
    c = BANNER[theme]
    top, w, h = 30, 1280, 326  # viewBox; the channels span y = 60..326
    centre = top + h / 2
    body = []
    if background:
        body.append(f'<rect y="{top}" width="{w}" height="{h}" fill="{background}"/>')
    lines = "".join(f"M640,{y}H1220" for y in range(60, 327, 14))
    body.append(f'<path d="{lines}" stroke="{c["grid"]}" stroke-width="2"/>')
    body.append(
        f'<path d="M680 60C690 200,800 300,1040 326" fill="none" stroke="{c["sweep"]}" '
        'stroke-opacity="0.5" stroke-width="7" stroke-linecap="round" '
        'stroke-dasharray="0.1 14"/>'
    )
    body.append(
        f'<path d="M1068,200H1112M1102,192L1112,200L1102,208" fill="none" '
        f'stroke="{c["arrow"]}" stroke-width="2" stroke-linecap="round"/>'
    )
    body.append(
        f'<path d="M1150,60V326" stroke="{c["pulse"]}" stroke-width="7" '
        'stroke-linecap="round"/>'
    )
    # The small white flake marks the frozen burst. On the light banner a
    # glacier outline keeps its diagonals visible against the white page.
    flake = (
        "M1150 193V181M1150 207V219M1144 196L1134 190M1156 196L1166 190"
        "M1144 204L1134 210M1156 204L1166 210"
    )
    if c["halo"]:
        body.append(
            f'<path d="{flake}" fill="none" stroke="{c["halo"]}" '
            'stroke-width="6" stroke-linecap="round"/>'
        )
    body.append(
        f'<path d="{flake}" fill="none" stroke="{WHITE}" '
        'stroke-width="3" stroke-linecap="round"/>'
    )

    # Left column, centred on the channels: lockup row, then the expansion.
    row, sub, gap = 96, 15, 22
    sub_h = sub * 1.3
    y0 = centre - (row + gap + sub_h) / 2
    body.append(placed_mark(80, y0, row, c["mark"], c["dot"]))
    words, _ = wordmark(sora, 88, 80 + row + 20, y0 + row / 2, c["name"])
    body.append(words)
    y = y0 + row + gap
    body.append(
        f'<path d="{mono.outline(EXPANSION, sub, 80, y + sub_h / 2 + 0.36 * sub, 0.02)}" '
        f'fill="{c["sub"]}"/>'
    )
    return svg((0, top, w, h), "".join(body), f"{NAME}: {EXPANSION}")


def social_svg(banner: str) -> str:
    """1280x640 GitHub social preview: the dark banner centred on polar night."""
    nested = banner.replace("<svg ", '<svg x="0" y="157" width="1280" height="326" ', 1)
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

    sora, mono = Face("sora"), Face("plex-mono")
    banner = banner_svg(sora, mono, "light")
    banner_dark = banner_svg(sora, mono, "dark")

    # Files referenced by README.md, docs/conf.py and docs/index.md.
    print("docs/_static/logo:")
    write(STATIC / "frost-banner.svg", banner)
    write(STATIC / "frost-banner-dark.svg", banner_dark)
    write(STATIC / "frost-logo.svg", lockup_svg(sora, NIGHT, GLACIER, ICE))
    write(STATIC / "frost-logo-dark.svg", lockup_svg(sora, WHITE, ICE, WHITE))
    write(STATIC / "frost-mark.svg", mark_svg(GLACIER, ICE))
    write(STATIC / "frost-mark-dark.svg", mark_svg(ICE, WHITE))
    write(STATIC / "favicon.svg", favicon_svg())

    if not args.exports:
        return

    print("docs/logo-src/build:")
    assets = {
        "frost-banner": (banner, 2560),
        "frost-banner-dark": (banner_svg(sora, mono, "dark", NIGHT), 2560),
        "frost-logo": (lockup_svg(sora, NIGHT, GLACIER, ICE), 2400),
        "frost-logo-dark": (lockup_svg(sora, WHITE, ICE, WHITE), 2400),
        "frost-mark": (mark_svg(GLACIER, ICE), 1024),
        "frost-mark-dark": (mark_svg(ICE, WHITE), 1024),
        # One colour, for print and stickers.
        "frost-mark-mono": (mark_svg(NIGHT, None), 1024),
        "frost-mark-mono-dark": (mark_svg(WHITE, None), 1024),
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
        Image.open(io.BytesIO(png(small, s, s))).convert("RGBA") for s in (64, 48, 32, 16)
    ]
    buf = io.BytesIO()
    frames[0].save(
        buf, format="ICO", sizes=[(s, s) for s in (16, 32, 48, 64)], append_images=frames[1:]
    )
    write(BUILD / "favicon.ico", buf.getvalue())
    write(BUILD / "social-preview.png", png(social_svg(banner_dark), 1280, 640))


if __name__ == "__main__":
    main()
