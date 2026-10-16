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

from dc2_strokes import strokes_for  # noqa: F401 (also used by the font builder)

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


# Ligatures made of two HD glyphs; the hand-drawn glyphs are in dc2_strokes.py.
LIGATURES = {"æ": ("a", "e"), "œ": ("o", "e"), "Æ": ("A", "E"), "Œ": ("O", "E")}


def manual_mask(strokes, pen: float, dx: float = 0.0) -> np.ndarray:
    shapes = []
    for stroke in strokes:
        if isinstance(stroke, tuple):  # A dot, a little rounder than the pen.
            centre = ((stroke[1] + PAD + dx) * SCALE, (stroke[2] + PAD) * SCALE)
            radius = stroke[3] if len(stroke) > 3 else pen * 0.62
            shapes.append(Point(centre).buffer(radius * SCALE, quad_segs=16))
            continue
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
        if (strokes := strokes_for(char)) is not None:
            result = manual_mask(strokes, self.pen)
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
