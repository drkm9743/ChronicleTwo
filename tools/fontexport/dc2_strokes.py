"""Hand-drawn centre lines for the HD style, following the retail font's design.

The retail Western font is a condensed gothic: a 12 texel cap height, about 7 texels
wide, round bowls and straight diagonals. At that size many glyphs can't be
recovered cleanly from the texels, so the letters, digits and common punctuation
are redrawn here as pen strokes in the same proportions and drawn with the one
pen width the HD style measures from the retail glyphs.

Coordinates are texels in the 8x20 cell, y down, on the stroke's centre line.
Nothing here is copied from the game; it is a drawing that imitates its style.
"""

from __future__ import annotations

import numpy as np

L, R, C = 1.1, 6.9, 4.0  # Left and right stems, centre.
T, B = 5.1, 15.4  # Cap and ascender top, baseline.
M = (T + B) / 2  # Middle of the capitals.
X = 8.75  # x-height.
XM = (X + B) / 2  # Middle of the lowercase.
D = 17.75  # Descender.
POWER = 2.3  # Squareness of the round strokes: 2 is an ellipse.


def arc(cx, cy, rx, ry, start, end, steps=40, power=POWER):
    """Superellipse arc. Angles in degrees: 0 right, 90 down, 180 left, 270 up."""
    out = []
    for t in np.linspace(np.radians(start), np.radians(end), steps):
        c, s = np.cos(t), np.sin(t)
        out.append((cx + rx * np.sign(c) * abs(c) ** (2 / power), cy + ry * np.sign(s) * abs(s) ** (2 / power)))
    return out


def ring(cx, cy, rx, ry, power=POWER):
    return arc(cx, cy, rx, ry, 0, 360, 72, power)


def dot(x, y):
    return ("dot", x, y)


def _move(strokes, dx=0.0, dy=0.0):
    out = []
    for stroke in strokes:
        if isinstance(stroke, tuple):
            out.append(("dot", stroke[1] + dx, stroke[2] + dy) + stroke[3:])
        else:
            out.append([(x + dx, y + dy) for x, y in stroke])
    return out


def _turn(strokes, cx, cy):
    """Rotated half a turn about (cx, cy), for ¡ and ¿."""
    out = []
    for stroke in strokes:
        if isinstance(stroke, tuple):
            out.append(("dot", 2 * cx - stroke[1], 2 * cy - stroke[2]))
        else:
            out.append([(2 * cx - x, 2 * cy - y) for x, y in stroke])
    return out


BOWL = ring(C, XM, 2.9, (B - X) / 2)  # The lowercase o.

GLYPHS: dict[str, list] = {
    # Capitals.
    "A": [[(L, B), (C, T), (R, B)], [(2.05, 12.0), (5.95, 12.0)]],
    "B": [[(L, B), (L, T), (4.6, T)] + arc(4.6, (T + M) / 2, 1.9, (M - T) / 2, 270, 450, power=2.2) + [(L, M)],
          [(L, M), (4.7, M)] + arc(4.7, (M + B) / 2, 2.2, (B - M) / 2, 270, 450, power=2.2) + [(L, B)]],
    "C": [arc(C, M, 2.9, (B - T) / 2, 325, 35)],
    "D": [[(L, T), (L, B)], [(L, T), (3.3, T)] + arc(3.3, M, R - 3.3, (B - T) / 2, 270, 450, power=2.6) + [(L, B)]],
    "E": [[(R, T), (L, T), (L, B), (R, B)], [(L, M), (6.2, M)]],
    "F": [[(R, T), (L, T), (L, B)], [(L, M), (6.2, M)]],
    "G": [arc(C, M, 2.9, (B - T) / 2, 325, 15) + [(R, M + 0.7), (4.3, M + 0.7)]],
    "H": [[(L, T), (L, B)], [(R, T), (R, B)], [(L, M), (R, M)]],
    "I": [[(L, T), (L, B)]],
    "J": [[(R - 0.3, T), (R - 0.3, 12.4)] + arc(3.85, 12.4, 2.75, 3.0, 0, 165, power=2.2)],
    "K": [[(L, T), (L, B)], [(R, T), (L, 11.3)], [(3.0, 9.7), (R, B)]],
    "L": [[(L, T), (L, B), (R, B)]],
    "M": [[(L, B), (L, T), (4.4, 11.6), (7.7, T), (7.7, B)]],
    "N": [[(L, B), (L, T), (R, B), (R, T)]],
    "O": [ring(C, M, 2.9, (B - T) / 2)],
    "P": [[(L, B), (L, T), (4.4, T)] + arc(4.4, 7.95, 2.5, 2.85, 270, 450, power=2.2) + [(L, 10.8)]],
    "Q": [ring(C, M, 2.9, (B - T) / 2), [(4.6, 12.7), (7.3, 16.2)]],
    "R": [[(L, B), (L, T), (4.4, T)] + arc(4.4, 7.85, 2.5, 2.75, 270, 450, power=2.2) + [(L, 10.6)],
          [(3.9, 10.6), (R, B)]],
    "S": [arc(C, 7.62, 2.75, 2.52, 335, 90, power=2.0) + arc(C, 12.77, 2.9, 2.63, 270, 515, power=2.0)],
    "T": [[(0.9, T), (7.1, T)], [(C, T), (C, B)]],
    "U": [[(L, T), (L, 12.3)] + arc(C, 12.3, 2.9, 3.1, 180, 0) + [(R, T)]],
    "V": [[(L, T), (C, B), (R, T)]],
    "W": [[(0.9, T), (2.6, B), (4.4, 8.4), (6.2, B), (7.9, T)]],
    "X": [[(L, T), (R, B)], [(R, T), (L, B)]],
    "Y": [[(L, T), (C, 10.6), (R, T)], [(C, 10.6), (C, B)]],
    "Z": [[(1.3, T), (R, T), (L, B), (R, B)]],
    # Lowercase.
    "a": [[(1.6, 9.5)] + arc(C, 11.0, 2.9, 2.25, 220, 360, power=2.2) + [(R, B)],
          [(R, 11.9), (3.9, 11.9)] + arc(3.9, 13.65, 2.8, 1.75, 270, 90, power=2.2)
          + arc(4.9, 13.4, 2.0, 2.0, 90, 0, 16, power=2.0)[1:]],
    "b": [[(L, T), (L, B)], BOWL],
    "c": [arc(C, XM, 2.9, (B - X) / 2, 325, 35)],
    "d": [[(R, T), (R, B)], BOWL],
    "e": [[(L, XM), (R, XM)] + arc(C, XM, 2.9, (B - X) / 2, 360, 38)],
    "f": [[(2.7, B), (2.7, 7.2)] + arc(4.5, 7.2, 1.8, 2.1, 180, 325, power=2.2), [(L, X), (5.3, X)]],
    "g": [ring(C, 11.9, 2.9, 3.15), [(R, X), (R, 16.3)] + arc(C, 16.3, 2.9, 1.45, 0, 165, power=2.2)],
    "h": [[(L, T), (L, B)], arc(C, 11.2, 2.9, 2.45, 180, 360, power=2.2) + [(R, B)]],
    "i": [[(1.4, X), (1.4, B)], dot(1.4, 6.0)],
    "j": [[(3.0, X), (3.0, 16.4)] + arc(1.7, 16.4, 1.3, 1.35, 0, 150, power=2.2), dot(3.0, 6.0)],
    "k": [[(L, T), (L, B)], [(6.3, X), (L, 12.8)], [(3.0, 11.3), (6.6, B)]],
    "l": [[(1.4, T), (1.4, B)]],
    "m": [[(L, X), (L, B)], arc(2.85, 11.0, 1.75, 2.25, 180, 360, power=2.2) + [(4.6, B)],
          arc(6.35, 11.0, 1.75, 2.25, 180, 360, power=2.2) + [(8.1, B)]],
    "n": [[(L, X), (L, B)], arc(C, 11.2, 2.9, 2.45, 180, 360, power=2.2) + [(R, B)]],
    "o": [BOWL],
    "p": [[(L, X), (L, D)], BOWL],
    "q": [[(R, X), (R, D)], BOWL],
    "r": [[(L, X), (L, B)], arc(4.4, 11.5, 3.3, 2.75, 180, 305, power=2.2)],
    "s": [arc(C, 10.35, 2.6, 1.6, 335, 90, power=2.0) + arc(C, 13.675, 2.75, 1.725, 270, 515, power=2.0)],
    "t": [[(2.6, 6.4), (2.6, 13.8)] + arc(4.3, 13.8, 1.7, 1.6, 180, 70, power=2.2), [(L, X), (5.4, X)]],
    "u": [[(L, X), (L, 12.3)] + arc(C, 12.3, 2.9, 3.1, 180, 0, power=2.2), [(R, X), (R, B)]],
    "v": [[(L, X), (C, B), (R, X)]],
    "w": [[(0.9, X), (2.6, B), (4.4, 10.4), (6.2, B), (7.9, X)]],
    "x": [[(L, X), (R, B)], [(R, X), (L, B)]],
    "y": [[(L, X), (3.89, 14.9)], [(R, X), (2.5, D)]],
    "z": [[(1.3, X), (R, X), (L, B), (R, B)]],
    # Digits (tabular in the font builder).
    "0": [ring(C, M, 2.75, (B - T) / 2, 2.4)],
    "1": [[(2.4, 6.7), (4.1, T), (4.1, B)]],
    "2": [arc(C, 7.85, 2.8, 2.75, 195, 380, power=2.0) + [(L, B), (R, B)]],
    "3": [arc(C, 7.62, 2.7, 2.52, 205, 450, power=2.0) + arc(C, 12.77, 2.9, 2.63, 270, 515, power=2.0)],
    "4": [[(5.3, B), (5.3, T), (L, 12.3), (7.1, 12.3)]],
    "5": [[(6.4, T), (1.7, T), (1.5, 10.1)] + arc(4.0, 12.6, 2.9, 2.8, 228, 515, power=2.0)],
    "6": [ring(C, 12.55, 2.9, 2.85, 2.1), [(L, 12.6), (L, 9.4)] + arc(4.4, 9.4, 3.3, 4.3, 180, 300, power=2.0)],
    "7": [[(L, T), (R, T), (3.3, B)]],
    "8": [ring(C, 7.67, 2.6, 2.57, 2.1), ring(C, 12.8, 2.9, 2.6, 2.1)],
    "9": [ring(C, 7.95, 2.9, 2.85, 2.1), [(R, 7.9), (R, 11.1)] + arc(3.6, 11.1, 3.3, 4.3, 0, 120, power=2.0)],
    # Punctuation and symbols.
    ".": [dot(1.3, B - 0.15)],
    ",": [[(1.7, B - 0.6), (1.7, B + 0.3), (1.0, 16.9)]],
    ":": [dot(1.3, X + 0.6), dot(1.3, B - 0.15)],
    ";": [dot(1.7, X + 0.6), [(1.7, B - 0.6), (1.7, B + 0.3), (1.0, 16.9)]],
    "!": [[(1.3, T), (1.3, 12.3)], dot(1.3, B - 0.15)],
    "?": [arc(C, 7.75, 2.75, 2.65, 195, 430, power=2.0) + [(C, 11.4), (C, 12.3)], dot(C, B - 0.15)],
    "-": [[(1.4, 11.3), (6.6, 11.3)]],
    "_": [[(0.6, 17.2), (7.4, 17.2)]],
    "'": [[(2.0, 7.2), (2.9, 4.9)]],
    '"': [[(1.6, 5.0), (1.6, 7.4)], [(3.7, 5.0), (3.7, 7.4)]],
    "(": [arc(6.1, 10.6, 3.6, 6.2, 125, 235, power=2.0)],
    ")": [arc(1.9, 10.6, 3.6, 6.2, -55, 55, power=2.0)],
    "[": [[(5.0, 4.6), (2.6, 4.6), (2.6, 16.6), (5.0, 16.6)]],
    "]": [[(3.0, 4.6), (5.4, 4.6), (5.4, 16.6), (3.0, 16.6)]],
    "/": [[(1.3, 16.4), (6.7, 4.6)]],
    "|": [[(1.4, 4.4), (1.4, 17.0)]],
    "+": [[(1.3, 11.3), (6.7, 11.3)], [(C, 8.6), (C, 14.0)]],
    "=": [[(1.3, 9.9), (6.7, 9.9)], [(1.3, 12.7), (6.7, 12.7)]],
    "<": [[(6.4, 7.5), (1.6, 11.3), (6.4, 15.1)]],
    ">": [[(1.6, 7.5), (6.4, 11.3), (1.6, 15.1)]],
    "*": [[(C, 7.8), (C, 13.4)], [(1.6, 9.2), (6.4, 12.0)], [(6.4, 9.2), (1.6, 12.0)]],
    "#": [[(2.9, 6.0), (2.5, 16.0)], [(5.7, 6.0), (5.3, 16.0)], [(1.0, 9.2), (7.1, 9.2)], [(0.9, 12.8), (7.0, 12.8)]],
    "%": [ring(2.4, 7.5, 1.3, 1.9, 2.0), ring(6.4, 13.0, 1.3, 1.9, 2.0), [(7.3, T), (1.5, B)]],
    "@": [[(6.1, 9.0), (6.1, 12.2), (6.7, 12.9), (7.5, 12.8)] + arc(4.5, 10.6, 3.6, 4.7, 375, 70, 64, 2.2),
          ring(4.7, 10.85, 1.4, 1.65, 2.0)],
    "$": [arc(C, 8.1, 2.6, 2.1, 335, 90, power=2.0) + arc(C, 12.3, 2.75, 2.1, 270, 515, power=2.0),
          [(C, 4.4), (C, 16.0)]],
    "&": [[(6.9, B), (2.5, 9.9)] + arc(3.6, 7.5, 1.55, 2.0, 135, 405, power=2.0) + [(1.9, 11.7)]
          + arc(3.6, 13.0, 2.4, 2.4, 215, 20, power=2.0) + [(7.0, 11.8)]],
    "~": [[(1.2, 11.6)] + [(1.2 + 5.6 * t, 11.3 - 0.8 * np.sin(2 * np.pi * t)) for t in np.linspace(0, 1, 24)][1:]],
    "^": [[(2.2, 6.9), (C, 4.9), (5.8, 6.9)]],
    "`": [[(3.0, 4.9), (4.6, 6.7)]],
    "{": [[(5.4, 4.6), (4.2, 4.9), (3.9, 6.0), (3.9, 9.6), (2.4, 10.6), (3.9, 11.6), (3.9, 15.2), (4.2, 16.3), (5.4, 16.6)]],
    "}": [[(2.6, 4.6), (3.8, 4.9), (4.1, 6.0), (4.1, 9.6), (5.6, 10.6), (4.1, 11.6), (4.1, 15.2), (3.8, 16.3), (2.6, 16.6)]],
    "°": [ring(3.0, 6.7, 1.5, 1.5, 2.0)],
    "º": [ring(3.3, 7.0, 1.7, 1.9, 2.0), [(1.4, 10.3), (5.2, 10.3)]],
    "ª": [ring(3.0, 7.3, 1.5, 1.6, 2.0), [(4.5, 5.6), (4.5, 8.9)], [(1.3, 10.4), (5.0, 10.4)]],
    "ß": [[(L, B), (L, 7.6)] + arc(3.6, 7.6, 2.5, 2.5, 180, 420, power=2.0) + [(3.4, 10.4)]
          + arc(3.6, 12.85, 3.0, 2.55, 270, 490, power=2.0)],
}
GLYPHS["¡"] = [dot(1.3, 8.9), [(1.3, 11.4), (1.3, 17.6)]]
GLYPHS["¿"] = _turn(GLYPHS["?"], C, 11.6)
GLYPHS["’"] = [[(2.4, 4.9), (2.4, 5.8), (1.7, 7.4)]]
GLYPHS["‘"] = _turn(GLYPHS["’"], 2.05, 6.15)
GLYPHS["”"] = GLYPHS["’"] + _move(GLYPHS["’"], 2.2)
GLYPHS["“"] = GLYPHS["‘"] + _move(GLYPHS["‘"], 2.2)
# The full-width symbols of the EU font (16 texel cells).
GLYPHS["○"] = [ring(8.0, 10.6, 5.4, 5.4, 2.0)]
GLYPHS["●"] = [("dot", 8.0, 10.6, 6.0)]
GLYPHS["・"] = [("dot", 4.0, 11.0, 1.0)]
GLYPHS["↑"] = [[(8.0, 4.8), (8.0, 16.6)], [(4.4, 8.4), (8.0, 4.8), (11.6, 8.4)]]
GLYPHS["↓"] = [[(8.0, 4.8), (8.0, 16.6)], [(4.4, 13.0), (8.0, 16.6), (11.6, 13.0)]]
GLYPHS["←"] = [[(2.2, 10.8), (13.8, 10.8)], [(5.8, 7.2), (2.2, 10.8), (5.8, 14.4)]]
GLYPHS["→"] = [[(2.2, 10.8), (13.8, 10.8)], [(10.2, 7.2), (13.8, 10.8), (10.2, 14.4)]]
for _narrow, _wide in zip("0123456789:+-", "０１２３４５６７８９：＋－"):
    GLYPHS[_wide] = GLYPHS[_narrow]

# Accents: name -> strokes around (0, 0), placed at the base letter's centre.
MARKS = {
    "acute": [[(-0.75, 0.85), (0.85, -0.85)]],
    "grave": [[(-0.85, -0.85), (0.75, 0.85)]],
    "circumflex": [[(-1.6, 0.8), (0.0, -0.9), (1.6, 0.8)]],
    "tilde": [[(-2.2 + 4.4 * t, -0.65 * np.sin(2 * np.pi * t)) for t in np.linspace(0, 1, 24)]],
    "diaeresis": [dot(-1.4, 0.0), dot(1.4, 0.0)],
    "ring": [ring(0.0, -0.2, 1.15, 1.15, 2.0)],
}
LOWER_MARK, CAP_MARK = 6.3, 2.9  # Height of the accent's centre.
ACCENTED = {
    "À": ("A", "grave"), "Á": ("A", "acute"), "Â": ("A", "circumflex"), "Ã": ("A", "tilde"),
    "Ä": ("A", "diaeresis"), "Å": ("A", "ring"), "È": ("E", "grave"), "É": ("E", "acute"),
    "Ê": ("E", "circumflex"), "Ë": ("E", "diaeresis"), "Ì": ("I", "grave"), "Í": ("I", "acute"),
    "Î": ("I", "circumflex"), "Ï": ("I", "diaeresis"), "Ñ": ("N", "tilde"), "Ò": ("O", "grave"),
    "Ó": ("O", "acute"), "Ô": ("O", "circumflex"), "Õ": ("O", "tilde"), "Ö": ("O", "diaeresis"),
    "Ù": ("U", "grave"), "Ú": ("U", "acute"), "Û": ("U", "circumflex"), "Ü": ("U", "diaeresis"),
    "Ý": ("Y", "acute"),
    "à": ("a", "grave"), "á": ("a", "acute"), "â": ("a", "circumflex"), "ã": ("a", "tilde"),
    "ä": ("a", "diaeresis"), "å": ("a", "ring"), "è": ("e", "grave"), "é": ("e", "acute"),
    "ê": ("e", "circumflex"), "ë": ("e", "diaeresis"), "ì": ("ı", "grave"), "í": ("ı", "acute"),
    "î": ("ı", "circumflex"), "ï": ("ı", "diaeresis"), "ñ": ("n", "tilde"), "ò": ("o", "grave"),
    "ó": ("o", "acute"), "ô": ("o", "circumflex"), "õ": ("o", "tilde"), "ö": ("o", "diaeresis"),
    "ù": ("u", "grave"), "ú": ("u", "acute"), "û": ("u", "circumflex"), "ü": ("u", "diaeresis"),
    "ý": ("y", "acute"), "ÿ": ("y", "diaeresis"),
}
GLYPHS["ı"] = [[(1.4, X), (1.4, B)]]
CEDILLA = [[(0.0, 0.0), (0.0, 1.0), (0.9, 1.5), (0.5, 2.5), (-0.7, 2.6)]]


def _centre(strokes) -> float:
    points = [q for s in strokes if not isinstance(s, tuple) for q in s]
    points += [(s[1], s[2]) for s in strokes if isinstance(s, tuple)]
    return (min(p[0] for p in points) + max(p[0] for p in points)) / 2


def strokes_for(char: str) -> list | None:
    if char in GLYPHS:
        return GLYPHS[char]
    if char in ACCENTED:
        base, mark = ACCENTED[char]
        strokes = GLYPHS[base]
        y = CAP_MARK if base.isupper() else LOWER_MARK
        return strokes + _move(MARKS[mark], _centre(strokes), y)
    if char in "Çç":
        strokes = GLYPHS["C" if char == "Ç" else "c"]
        return strokes + _move(CEDILLA, C, B + 0.2)
    return None
