"""HD reconstruction of the game's small monoline glyphs.

The retail glyphs are about 8x12 texels with 4-bit antialiasing, so tracing their
edges directly gives wobbly outlines with uneven stroke width. Instead this
recovers each glyph's centre lines (skeleton of the upsampled glyph), smooths
them, and draws them again with one constant pen width, square ends and round
joins. The result is rasterised finely and traced with potrace into curves.
"""

from __future__ import annotations

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage
from shapely.geometry import LineString, MultiPolygon, Point, Polygon
from shapely.ops import unary_union
from skimage.morphology import skeletonize

SCALE = 16  # Working resolution: pixels per texel.
PAD = 2  # Texels of padding around the cell.

_NEIGHBOURS = [(-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1)]


def _upsample(alpha: np.ndarray, blur: float = 0.25) -> np.ndarray:
    h, w = alpha.shape
    image = Image.fromarray(np.pad(alpha, PAD).astype(np.uint8), "L")
    big = image.resize(((w + PAD * 2) * SCALE, (h + PAD * 2) * SCALE), Image.BICUBIC)
    return ndimage.gaussian_filter(np.asarray(big).astype(float), SCALE * blur)


def _paths(skeleton: np.ndarray) -> list[list[tuple[int, int]]]:
    """Splits a one pixel skeleton into paths between end points and junctions."""
    points = set(zip(*np.nonzero(skeleton)))

    def neighbours(p):
        return [(p[0] + dy, p[1] + dx) for dy, dx in _NEIGHBOURS if (p[0] + dy, p[1] + dx) in points]

    degree = {p: len(neighbours(p)) for p in points}
    nodes = {p for p, d in degree.items() if d != 2}
    used = set()
    paths = []
    for start in nodes:
        for nxt in neighbours(start):
            edge = frozenset((start, nxt))
            if edge in used:
                continue
            path = [start, nxt]
            used.add(edge)
            while path[-1] not in nodes:
                options = [n for n in neighbours(path[-1]) if n != path[-2] and frozenset((path[-1], n)) not in used]
                if not options:
                    break
                # Prefer 4-connected steps so diagonal shortcuts don't skip pixels.
                options.sort(key=lambda n: abs(n[0] - path[-1][0]) + abs(n[1] - path[-1][1]))
                used.add(frozenset((path[-1], options[0])))
                path.append(options[0])
            paths.append(path)
    # Closed loops with no nodes (O, 0).
    remaining = {p for p in points if not any(frozenset((p, n)) in used for n in neighbours(p))}
    while remaining:
        start = remaining.pop()
        path = [start]
        while True:
            options = [n for n in neighbours(path[-1]) if n in remaining]
            if not options:
                break
            options.sort(key=lambda n: abs(n[0] - path[-1][0]) + abs(n[1] - path[-1][1]))
            remaining.discard(options[0])
            path.append(options[0])
        path.append(start)
        paths.append(path)
    return paths


def _prune(paths, nodes_degree, min_length):
    """Drops short spurs: paths with a free end that are shorter than min_length."""
    counts = {}
    for path in paths:
        for end in (path[0], path[-1]):
            counts[end] = counts.get(end, 0) + 1
    kept = []
    for path in paths:
        free = counts[path[0]] == 1 or counts[path[-1]] == 1
        joined = counts[path[0]] > 1 or counts[path[-1]] > 1
        if free and joined and len(path) < min_length:
            continue
        kept.append(path)
    return kept


def _smooth(path: list[tuple[int, int]], sigma: float) -> np.ndarray:
    points = np.array([(x, y) for y, x in path], dtype=float)
    closed = len(points) > 3 and tuple(points[0]) == tuple(points[-1])
    if len(points) < 3:
        return points
    if closed:
        body = ndimage.gaussian_filter1d(points[:-1], sigma, axis=0, mode="wrap")
        return np.vstack([body, body[:1]])
    smooth = ndimage.gaussian_filter1d(points, sigma, axis=0, mode="nearest")
    smooth[0], smooth[-1] = points[0], points[-1]
    return smooth


def _holes(mask: np.ndarray) -> int:
    """Enclosed background regions (counters) of a mask, ignoring specks."""
    labels, count = ndimage.label(~mask)
    border = set(np.unique(np.concatenate([labels[0], labels[-1], labels[:, 0], labels[:, -1]])))
    sizes = ndimage.sum(np.ones_like(labels), labels, range(1, count + 1))
    return sum(1 for i, size in enumerate(sizes, 1) if i not in border and size >= SCALE * SCALE * 0.15)


CANVAS_W = 16 + PAD * 2  # Texels; wide enough for the widened HD glyphs.
CANVAS_H = 20 + PAD * 2


def _to_canvas(mask: np.ndarray) -> np.ndarray:
    canvas = np.zeros((CANVAS_H * SCALE, CANVAS_W * SCALE), bool)
    canvas[:mask.shape[0], :mask.shape[1]] = mask[:CANVAS_H * SCALE, :CANVAS_W * SCALE]
    return canvas


def _draw_shape(shape, size) -> np.ndarray:
    canvas = Image.new("L", size, 0)
    draw = ImageDraw.Draw(canvas)
    for polygon in shape.geoms if isinstance(shape, MultiPolygon) else [shape]:
        if not isinstance(polygon, Polygon) or polygon.is_empty:
            continue
        draw.polygon([tuple(p) for p in polygon.exterior.coords], fill=255)
        for hole in polygon.interiors:
            draw.polygon([tuple(p) for p in hole.coords], fill=0)
    return np.asarray(canvas) >= 128


def _render(field: np.ndarray, threshold: float, pen: float) -> np.ndarray | None:
    mask = field >= threshold
    if not mask.any():
        return None
    skeleton = skeletonize(mask)
    paths = _prune(_paths(skeleton), None, min_length=pen * 0.55)
    shapes = []
    for path in paths:
        line = _smooth(path, sigma=SCALE * 0.45)
        if len(line) < 2 or LineString(line).length < pen * 0.3:
            shapes.append(Point(line.mean(axis=0)).buffer(pen / 2, quad_segs=16))
        else:
            shapes.append(LineString(line).buffer(pen / 2, cap_style="round", join_style="round", quad_segs=16))
    if not shapes:
        return None
    return _draw_shape(unary_union(shapes), (field.shape[1], field.shape[0]))


def auto_mask(alpha: np.ndarray, pen: float) -> np.ndarray | None:
    """Skeleton reconstruction of a retail glyph, on the canvas.

    Tries a few skeleton thresholds, blurs and pen widths and keeps the one whose
    coverage, averaged back down to the texel grid, best matches the retail glyph
    with the same number of counters.
    """
    source = np.pad(alpha, PAD).astype(float) / 255
    h, w = source.shape
    reference = np.asarray(Image.fromarray(np.pad(alpha, PAD).astype(np.uint8), "L")
                           .resize((w * SCALE, h * SCALE), Image.BICUBIC)) >= 128
    want_holes = _holes(reference)
    base = pen * SCALE
    best, best_score = None, None
    for blur in (0.15, 0.25):
        field = _upsample(alpha, blur)
        for threshold in (128, 155, 185):
            for factor in (0.85, 1.0, 1.15):
                result = _render(field, threshold, base * factor)
                if result is None:
                    continue
                coverage = result.reshape(h, SCALE, w, SCALE).mean(axis=(1, 3))
                score = np.abs(coverage - source).mean() + 0.1 * abs(_holes(result) - want_holes)
                score += 0.002 * abs(factor - 1.0)  # Prefer the common pen when it fits as well.
                if best_score is None or score < best_score:
                    best, best_score = result, score
    return None if best is None else _to_canvas(best)


# --- Hand-drawn centre lines -------------------------------------------------------------------
# Glyphs whose retail versions are too cramped (8 texels for four diagonals) for the skeleton to
# recover. Coordinates are texels in the cell (y down, baseline 16); the cap height runs 5.5-14.8
# and the x-height 8.5-14.8 on the centre line. These are drawn for this tool, not copied.

CAP_T, X_T, BASE = 5.5, 8.5, 14.8


def _arc(cx, cy, rx, ry, start, end, steps=48, power=2.0):
    """Superellipse arc; angles in degrees, 0 = right, 90 = down (y down)."""
    out = []
    for t in np.linspace(np.radians(start), np.radians(end), steps):
        c, s = np.cos(t), np.sin(t)
        out.append((cx + rx * np.sign(c) * abs(c) ** (2 / power), cy + ry * np.sign(s) * abs(s) ** (2 / power)))
    return out


MANUAL = {
    "W": [[(1.2, CAP_T), (2.7, BASE), (4.6, 8.4), (6.5, BASE), (8.0, CAP_T)]],
    "w": [[(1.0, X_T), (2.5, BASE), (4.4, 10.2), (6.3, BASE), (7.8, X_T)]],
    "M": [[(1.2, BASE), (1.2, CAP_T), (4.4, 11.6), (7.6, CAP_T), (7.6, BASE)]],
    "N": [[(1.2, BASE), (1.2, CAP_T), (7.0, BASE), (7.0, CAP_T)]],
    "m": [[(1.2, X_T), (1.2, BASE)],
          [(1.2, 10.2)] + _arc(2.85, 10.2, 1.65, 1.7, 180, 360, 24) + [(4.5, BASE)],
          [(4.5, 10.2)] + _arc(6.15, 10.2, 1.65, 1.7, 180, 360, 24) + [(7.8, BASE)]],
    "%": [_arc(2.5, 7.4, 1.3, 1.9, 0, 360), _arc(6.5, 12.9, 1.3, 1.9, 0, 360), [(7.4, CAP_T), (1.6, BASE)]],
    "@": [_arc(5.0, 10.15, 3.8, 4.65, 30, 340, 64, 2.4) + [(8.6, 8.6), (8.6, 12.0)] + _arc(7.6, 12.0, 1.0, 1.0, 0, 90, 12),
          _arc(5.3, 10.6, 1.65, 1.75, 0, 360, 48)],
}
# Glyph made of the O plus a tail, and ligatures made of two reconstructed glyphs.
TAILS = {"Q": ("O", [[(4.9, 12.4), (7.6, 15.9)]])}
LIGATURES = {"æ": ("a", "e"), "œ": ("o", "e"), "Æ": ("A", "E"), "Œ": ("O", "E")}


def manual_mask(strokes, pen: float, dx: float = 0.0) -> np.ndarray:
    shapes = []
    for stroke in strokes:
        line = [((x + PAD + dx) * SCALE, (y + PAD) * SCALE) for x, y in stroke]
        shapes.append(LineString(line).buffer(pen * SCALE / 2, cap_style="round", join_style="round", quad_segs=16))
    return _draw_shape(unary_union(shapes), (CANVAS_W * SCALE, CANVAS_H * SCALE))


def _columns(mask: np.ndarray) -> tuple[int, int]:
    cols = np.nonzero(mask.any(axis=0))[0]
    return int(cols.min()), int(cols.max()) + 1


def _shift(mask: np.ndarray, pixels: int) -> np.ndarray:
    out = np.zeros_like(mask)
    if pixels >= 0:
        out[:, pixels:] = mask[:, :mask.shape[1] - pixels]
    else:
        out[:, :pixels] = mask[:, -pixels:]
    return out


class HDGlyphs:
    """Builds HD masks per character, with hand-drawn and composed exceptions."""

    def __init__(self, alpha_of, pen: float):
        self.alpha_of = alpha_of  # char -> retail alpha cell or None
        self.pen = pen
        self.cache = {}

    def mask(self, char: str) -> np.ndarray | None:
        if char in self.cache:
            return self.cache[char]
        if char in MANUAL:
            result = manual_mask(MANUAL[char], self.pen)
        elif char in TAILS:
            base, strokes = TAILS[char]
            body = self.mask(base)
            result = None if body is None else body | manual_mask(strokes, self.pen)
        elif char in LIGATURES:
            first, second = (self.mask(c) for c in LIGATURES[char])
            if first is None or second is None:
                result = None
            else:
                # The second letter starts where the first one's right stroke is, sharing it.
                _, right = _columns(first)
                left, _ = _columns(second)
                overlap = int(self.pen * SCALE)
                result = first | _shift(second, right - overlap - left)
        else:
            alpha = self.alpha_of(char)
            result = None if alpha is None or not alpha.any() else auto_mask(alpha, self.pen)
        self.cache[char] = result
        return result

    def contours(self, char: str) -> list[list]:
        mask = self.mask(char)
        return [] if mask is None else trace(mask)


def trace(mask: np.ndarray) -> list[list]:
    """Contours ('M'/'L'/'C' parts, texel coordinates, y down) of a canvas mask."""
    import potrace

    traced = potrace.Bitmap(~mask).trace(turdsize=SCALE * 4, alphamax=1.1, opttolerance=0.5)

    def texel(p):
        return (p.x / SCALE - PAD, p.y / SCALE - PAD)

    contours = []
    for curve in traced:
        parts = [("M", texel(curve.start_point))]
        for segment in curve.segments:
            if segment.is_corner:
                parts.append(("L", texel(segment.c)))
                parts.append(("L", texel(segment.end_point)))
            else:
                parts.append(("C", texel(segment.c1), texel(segment.c2), texel(segment.end_point)))
        contours.append(parts)
    return contours
