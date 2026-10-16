#!/usr/bin/env python3
"""Build TrueType fonts from the player's own extracted Dark Cloud 2 font textures.

Nothing from the game ships with this script: it reads the font atlas PNGs and
glyph tables that dc2data extracted on the player's machine (a run directory
with data/ and port_data/, or a data.zip) and writes .ttf files locally.

Fonts it knows about:
  eu  FontTex_2_0 + fonttbl_2.bin, the PAL French/German/Italian/Spanish font:
      ASCII, Latin-1 accents (À-ÿ, ¡ ¿ ª º ß), Œ œ and the symbol glyphs.
  en  FontTex_1_0 + fonttbl_1.bin, the English font: ASCII and the kana and
      symbols that fit on its first page.
  ko  FontTex_6_0/6_1 + fonttbl_6.bin, the Korean font: ASCII and the Hangul
      syllables the game's text uses (not the whole Hangul block).

Each font is written in two styles:
  Pixel   every texel becomes a square, so it matches the game exactly at
          whole multiples of 20 px and keeps the game's fixed 8 px advance.
  Smooth  the antialiased texels are upsampled and traced with potrace into
          curves, with proportional spacing for use as an ordinary font.

Requires: pip install fonttools potracer numpy pillow
"""

from __future__ import annotations

import argparse
import io
import struct
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image

CELL_W = 16  # Texels per font cell; half-width glyphs use the left 8.
CELL_H = 20
COLUMNS = 32
PAGE_GLYPHS = 0x260  # GetRectFontTex: 608 glyphs per page texture.
UNIT = 50  # Font units per texel: a 20 texel cell is a 1000 unit em.
BASELINE = 16  # Texel row the Latin glyphs stand on.

# alphabetical_chara_tbl (ps2/src/font.cpp): font numbers from 0x5E on.
EU_ACCENTS = [0xA1, 0xAA, 0xB0, 0xBA, 0xBF] + [c for c in range(0xC0, 0x100) if c not in (0xD0, 0xD7, 0xD8, 0xDE, 0xF0, 0xF7, 0xF8, 0xFE)] + [0x152, 0x153]
# FontGaijiConvTbl: font numbers 0x9D on, written in the text as Shift-JIS.
EU_SYMBOLS = "○●↑↓←→０１２３４５６７８９：＋－・“”‘’"
EU_ACCENT_TOP = 0x5E
EU_SYMBOL_TOP = 0x9D


@dataclass
class Glyph:
    char: str
    number: int  # Font number in the game's table.
    full: bool  # Full-width (16 texels) rather than half-width (8).


@dataclass
class FontSpec:
    key: str
    family: str
    table: str
    pages: list[str]


SPECS = {
    "eu": FontSpec("eu", "DC2 Chronicle EU", "meswin/fonttbl_2.bin", ["meswin/fonttex_2_0.tm2.img/"]),
    "en": FontSpec("en", "DC2 Chronicle EN", "meswin/fonttbl_1.bin", ["meswin/fonttex_1_0.tm2.img/"]),
    "ko": FontSpec("ko", "DC2 Chronicle KO", "meswin/fonttbl_6.bin",
                   ["meswin/fonttex_6_0.tm2.img/", "meswin/fonttex_6_1.tm2.img/"]),
}


class Source:
    """Reads extracted files from a run directory or a data.zip, matching by path suffix."""

    def __init__(self, root: Path):
        self.root = root
        if root.is_file():
            self.zip = zipfile.ZipFile(root)
            self.names = self.zip.namelist()
        else:
            self.zip = None
            self.names = [p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file()]
        self.lower = [n.lower() for n in self.names]

    def find(self, suffix: str, prefer: str, ext: str = "") -> str:
        suffix = suffix.lower()
        hits = [n for n, low in zip(self.names, self.lower)
                if (suffix in low if suffix.endswith("/") else low.endswith(suffix)) and low.endswith(ext)]
        hits.sort(key=lambda n: (prefer not in n.lower(), len(n)))
        if not hits:
            raise FileNotFoundError(f"no extracted file matches {suffix!r} in {self.root}")
        return hits[0]

    def read(self, name: str) -> bytes:
        return self.zip.read(name) if self.zip else (self.root / name).read_bytes()


def load_table(data: bytes) -> tuple[int, int, list[bytes]]:
    half_num, kanji_top, count, _ = struct.unpack_from("<4H", data)
    return half_num, kanji_top, [data[8 + i * 2:10 + i * 2] for i in range(count)]


def decode_code(key: str, code: bytes) -> str | None:
    if code[1] == 0x20 and 0x20 <= code[0] <= 0x7E:
        return chr(code[0])
    if key == "ko":
        value = (code[0] << 8) | code[1]
        return chr(value) if 0xAC00 <= value <= 0xD7A3 else None
    try:
        return code.decode("cp932")
    except UnicodeDecodeError:
        return None


def glyph_list(spec: FontSpec, table: bytes) -> list[Glyph]:
    half_num, _, codes = load_table(table)
    glyphs = []
    for number, code in enumerate(codes):
        char = decode_code(spec.key, code)
        if char and len(char) == 1:
            glyphs.append(Glyph(char, number, number >= half_num))
    if spec.key == "eu":
        glyphs += [Glyph(chr(c), EU_ACCENT_TOP + i, False) for i, c in enumerate(EU_ACCENTS)]
        glyphs += [Glyph(c, EU_SYMBOL_TOP + i, True) for i, c in enumerate(EU_SYMBOLS)]
    return glyphs


def cell(pages: list[np.ndarray], glyph: Glyph) -> np.ndarray | None:
    page, index = divmod(glyph.number, PAGE_GLYPHS)
    if page >= len(pages):
        return None
    atlas = pages[page]
    x, y = (index % COLUMNS) * CELL_W, (index // COLUMNS) * CELL_H
    if y + CELL_H > atlas.shape[0]:
        return None
    return atlas[y:y + CELL_H, x:x + (CELL_W if glyph.full else CELL_W // 2)]


# --- Outlines --------------------------------------------------------------------------------


def pixel_contours(mask: np.ndarray) -> list[list[tuple[float, float]]]:
    """Outline of the union of the set texels, as closed loops in texel coordinates (y down)."""
    edges = {}
    h, w = mask.shape
    for y in range(h):
        for x in range(w):
            if not mask[y, x]:
                continue
            # Clockwise on screen (y down) for filled squares.
            for a, b in (((x, y), (x + 1, y)), ((x + 1, y), (x + 1, y + 1)),
                         ((x + 1, y + 1), (x, y + 1)), ((x, y + 1), (x, y))):
                if (b, a) in edges:
                    del edges[(b, a)]
                else:
                    edges[(a, b)] = True
    nexts = {}
    for a, b in edges:
        nexts.setdefault(a, []).append(b)
    loops = []
    while nexts:
        start = next(iter(nexts))
        loop, point = [start], start
        while True:
            b = nexts[point].pop()
            if not nexts[point]:
                del nexts[point]
            if b == start:
                break
            loop.append(b)
            point = b
        # Drop collinear points.
        simple = [p for i, p in enumerate(loop)
                  if not _collinear(loop[i - 1], p, loop[(i + 1) % len(loop)])]
        loops.append(simple)
    return loops


def _collinear(a, b, c) -> bool:
    return (b[0] - a[0]) * (c[1] - b[1]) == (b[1] - a[1]) * (c[0] - b[0])


def smooth_contours(alpha: np.ndarray, scale: int = 8) -> list[list]:
    """Traced outline of the antialiased glyph: list of contours of ('L', p) / ('C', c1, c2, p) in texels."""
    import potrace

    h, w = alpha.shape
    pad = 2
    image = Image.fromarray(np.pad(alpha, pad).astype(np.uint8), "L")
    big = np.asarray(image.resize(((w + pad * 2) * scale, (h + pad * 2) * scale), Image.BICUBIC))
    # potracer traces the False texels.
    path = potrace.Bitmap(~(big >= 128)).trace(turdsize=scale * 2, alphamax=1.0, opttolerance=0.2)

    def texel(p):
        return (p.x / scale - pad, p.y / scale - pad)

    contours = []
    for curve in path:
        parts = [("M", texel(curve.start_point))]
        for segment in curve.segments:
            if segment.is_corner:
                parts.append(("L", texel(segment.c)))
                parts.append(("L", texel(segment.end_point)))
            else:
                parts.append(("C", texel(segment.c1), texel(segment.c2), texel(segment.end_point)))
        contours.append(parts)
    return contours


def _polygon(parts) -> list[tuple[float, float]]:
    return [p[-1] for p in parts]


def _area(points) -> float:
    return sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(points, points[1:] + points[:1])) / 2


def _inside(point, points) -> bool:
    x, y = point
    inside = False
    for (x1, y1), (x2, y2) in zip(points, points[1:] + points[:1]):
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            inside = not inside
    return inside


def oriented(contours: list[list]) -> list[list]:
    """Outer contours clockwise and holes counter-clockwise in font space (y up), by nesting depth."""
    polygons = [_polygon(c) for c in contours]
    result = []
    for i, contour in enumerate(contours):
        probe = polygons[i][0]
        depth = sum(1 for j, other in enumerate(polygons) if j != i and _inside(probe, other))
        # Texel space is y down, so a positive shoelace area there is clockwise on screen,
        # which flips to counter-clockwise once y points up.
        clockwise_in_font = _area(polygons[i]) < 0
        want_clockwise = depth % 2 == 0
        result.append(contour if clockwise_in_font == want_clockwise else _reverse(contour))
    return result


def _reverse(parts):
    points = [parts[0][1]]
    segments = []
    for part in parts[1:]:
        segments.append(part)
        points.append(part[-1])
    out = [("M", points[-1])]
    for k in range(len(segments) - 1, -1, -1):
        part, start = segments[k], points[k]
        out.append(("C", part[2], part[1], start) if part[0] == "C" else ("L", start))
    return out


# --- Font building ---------------------------------------------------------------------------


def draw(pen, contours, dx: float):
    def font(p):
        return (round((p[0] + dx) * UNIT), round((BASELINE - p[1]) * UNIT))

    for parts in contours:
        pen.moveTo(font(parts[0][1]))
        for part in parts[1:]:
            if part[0] == "L":
                pen.lineTo(font(part[1]))
            else:
                pen.curveTo(font(part[1]), font(part[2]), font(part[3]))
        pen.closePath()


def build(spec: FontSpec, style: str, glyphs: list[Glyph], pages: list[np.ndarray], out: Path):
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.cu2quPen import Cu2QuPen
    from fontTools.pens.ttGlyphPen import TTGlyphPen

    names = [".notdef", "space"]
    cmap = {0x20: "space"}
    outlines = {}
    metrics = {}
    space = 4 if style == "Smooth" else 8
    metrics["space"] = (space * UNIT, 0)
    digit_width = 0.0
    for glyph in glyphs:
        if glyph.char.isdigit() and ord(glyph.char) < 0x80 and (alpha := cell(pages, glyph)) is not None:
            cols = np.nonzero(alpha.max(axis=0) >= 48)[0]
            if cols.size:
                digit_width = max(digit_width, float(cols.max() + 1 - cols.min()))
    for glyph in glyphs:
        code = ord(glyph.char)
        if code in cmap or code == 0x20:
            continue
        alpha = cell(pages, glyph)
        if alpha is None or not alpha.any():
            continue
        width = alpha.shape[1]
        if style == "Pixel":
            contours = [[("M", loop[0])] + [("L", p) for p in loop[1:]] for loop in pixel_contours(alpha >= 128)]
            dx, advance = 0.0, width
        else:
            contours = smooth_contours(alpha)
            cols = np.nonzero(alpha.max(axis=0) >= 48)[0]
            left, right = float(cols.min()), float(cols.max() + 1)
            if glyph.char.isdigit() and code < 0x80:
                # Tabular digits: every digit takes the widest one's advance, centred.
                dx = 0.75 + (digit_width - (right - left)) / 2 - left
                advance = digit_width + 1.5
            else:
                dx, advance = 0.75 - left, (right - left) + 1.5
        if not contours:
            continue
        name = f"uni{code:04X}"
        names.append(name)
        cmap[code] = name
        outlines[name] = (oriented(contours), dx)
        metrics[name] = (round(advance * UNIT), 0)

    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap(cmap)
    tt_glyphs = {}
    notdef = TTGlyphPen(None)
    notdef.moveTo((50, 0))
    notdef.lineTo((50, 600))
    notdef.lineTo((350, 600))
    notdef.lineTo((350, 0))
    notdef.closePath()
    tt_glyphs[".notdef"] = notdef.glyph()
    metrics[".notdef"] = (400, 50)
    tt_glyphs["space"] = TTGlyphPen(None).glyph()
    for name, (contours, dx) in outlines.items():
        pen = TTGlyphPen(None)
        draw(Cu2QuPen(pen, max_err=1.0, reverse_direction=False), contours, dx)
        tt_glyphs[name] = pen.glyph()
    fb.setupGlyf(tt_glyphs)
    glyf = fb.font["glyf"]
    hmtx = {}
    for name in names:
        g = glyf[name]
        g.recalcBounds(glyf)
        hmtx[name] = (metrics[name][0], getattr(g, "xMin", 0))
    fb.setupHorizontalMetrics(hmtx)
    ascent, descent = BASELINE * UNIT, (CELL_H - BASELINE) * UNIT
    fb.setupHorizontalHeader(ascent=ascent, descent=-descent)
    family = f"{spec.family} {style}"
    fb.setupNameTable({
        "familyName": family,
        "styleName": "Regular",
        "uniqueFontIdentifier": f"{family.replace(' ', '')}-local",
        "fullName": family,
        "psName": family.replace(" ", ""),
        "version": "Version 0.1",
        "description": "Generated locally from the player's own Dark Cloud 2 data. Not for redistribution.",
    })
    fb.setupOS2(sTypoAscender=ascent, sTypoDescender=-descent, sTypoLineGap=0, usWinAscent=ascent,
                usWinDescent=descent, fsType=0x0002, achVendID="NONE")
    fb.setupPost(isFixedPitch=1 if style == "Pixel" else 0)
    fb.font["head"].unitsPerEm = 1000
    out.parent.mkdir(parents=True, exist_ok=True)
    fb.save(str(out))
    return len(cmap)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", type=Path, help="run directory (with data/ and port_data/) or data.zip")
    parser.add_argument("-o", "--out", type=Path, default=Path("fonts"), help="output directory")
    parser.add_argument("--font", choices=sorted(SPECS), action="append", help="font(s) to build (default: all)")
    parser.add_argument("--style", choices=["Pixel", "Smooth"], action="append", help="style(s) (default: both)")
    args = parser.parse_args()

    source = Source(args.source)
    for key in args.font or list(SPECS):
        spec = SPECS[key]
        try:
            table = source.read(source.find(spec.table, "data/"))
            pages = [np.asarray(Image.open(io.BytesIO(source.read(source.find(page, "port_data/", ".png"))))
                                .convert("RGBA"))[:, :, 3] for page in spec.pages]
        except FileNotFoundError as error:
            print(f"{key}: skipped, {error}", file=sys.stderr)
            continue
        glyphs = glyph_list(spec, table)
        for style in args.style or ["Pixel", "Smooth"]:
            out = args.out / f"{spec.family.replace(' ', '')}-{style}.ttf"
            count = build(spec, style, glyphs, pages, out)
            print(f"{out}: {count} characters")
    return 0


if __name__ == "__main__":
    sys.exit(main())
