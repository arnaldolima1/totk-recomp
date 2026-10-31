"""PlayStation face-button symbols for pad_layout = playstation (tools/make_romfs_patches.py --playstation-prompts).

The game draws a face-button prompt as a white disc with the button's letter cut out: as an alpha mask in the UI
textures (Nt_KeyTexA/B/X/Y_00) and as the glyphs U+E040 to U+E043 of its button font. This module draws the same disc
with a PlayStation symbol cut out instead (cross, circle, square, triangle), our own geometry, in both forms:

- mask(symbol, width, height, cx, cy, radius): 8-bit alpha rows, 4x4 supersampled, for a texture of that size whose
  disc the caller measured from the game's own texture;
- charstring(symbol, cx, cy, radius, width_operand): a Type 2 charstring (CFF) of the disc and the cut-out, outer
  contours counter-clockwise and holes clockwise, as the game's glyphs wind them.

The shapes live in a unit disc (radius 1, y up) and scale to either. Standard library only.
"""
import math

STROKE = 0.17                 # line width of every symbol, in disc radii
CROSS_ARM = 0.50              # half-length of each bar of the cross
RING = 0.44                   # centre-line radius of the circle
SQUARE = 0.36                 # half side of the square's centre line
TRIANGLE = 0.54               # circumradius of the triangle's centre line
TRIANGLE_DROP = 0.05          # the triangle sits a little low, so it looks centred
SYMBOLS = ('cross', 'circle', 'square', 'triangle')
KAPPA = 0.5522847498          # cubic Bezier handle length of a quarter circle


def _rotate(p, angle):
    c, s = math.cos(angle), math.sin(angle)
    return (p[0] * c - p[1] * s, p[0] * s + p[1] * c)


def _cross_polygon():
    """The cross as one 12-sided outline (a plus sign turned by 45 degrees), counter-clockwise."""
    h, a = STROKE / 2, CROSS_ARM
    plus = [(h, -h), (a, -h), (a, h), (h, h), (h, a), (-h, a), (-h, h), (-a, h), (-a, -h), (-h, -h), (-h, -a),
            (h, -a)]
    return [_rotate(p, math.pi / 4) for p in plus]


def _triangle(inradius):
    """An equilateral triangle pointing up with that inradius, counter-clockwise."""
    r = 2 * inradius
    cy = -TRIANGLE_DROP
    return [(r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a))) for a in (90, 210, 330)]


def _square(half):
    return [(half, -half), (half, half), (-half, half), (-half, -half)]   # counter-clockwise


# ---- the cut-out region, as a point test (masks)

def _in_polygon(p, poly):
    """Even-odd test of a point against a simple polygon."""
    x, y, inside = p[0], p[1], False
    for i in range(len(poly)):
        (x1, y1), (x2, y2) = poly[i - 1], poly[i]
        if (y1 > y) != (y2 > y) and x < x1 + (y - y1) * (x2 - x1) / (y2 - y1):
            inside = not inside
    return inside


def _cut(symbol):
    """A function (x, y) -> True where the symbol's line is, in unit-disc coordinates."""
    h = STROKE / 2
    if symbol == 'cross':
        poly = _cross_polygon()
        return lambda x, y: _in_polygon((x, y), poly)
    if symbol == 'circle':
        lo, hi = (RING - h) ** 2, (RING + h) ** 2
        return lambda x, y: lo <= x * x + y * y <= hi
    if symbol == 'square':
        lo, hi = SQUARE - h, SQUARE + h
        return lambda x, y: lo < max(abs(x), abs(y)) <= hi
    if symbol == 'triangle':
        outer, inner = _triangle(TRIANGLE / 2 + h), _triangle(TRIANGLE / 2 - h)
        return lambda x, y: _in_polygon((x, y), outer) and not _in_polygon((x, y), inner)
    raise ValueError(symbol)


def mask(symbol, width, height, cx, cy, radius, samples=4):
    """The disc with the symbol cut out as width * height alpha bytes, rows top to bottom. cx, cy and radius are in
    pixels with y down (the texture's own space)."""
    cut = _cut(symbol)
    out = bytearray(width * height)
    step = 1 / samples
    offsets = [(i + 0.5) * step for i in range(samples)]
    full = samples * samples
    for py in range(height):
        for px in range(width):
            hits = 0
            for oy in offsets:
                y = (cy - (py + oy)) / radius          # unit space, y up
                for ox in offsets:
                    x = (px + ox - cx) / radius
                    if x * x + y * y <= 1 and not cut(x, y):
                        hits += 1
            out[py * width + px] = (hits * 255 + full // 2) // full
    return bytes(out)


# ---- the same shapes as outlines (font)

def _circle_path(r, clockwise):
    """Four cubic segments of a circle of radius r starting at the top, as (start, [(c1, c2, end), ...])."""
    pts = [(0, r), (-r, 0), (0, -r), (r, 0)]              # top, left, bottom, right: counter-clockwise
    if clockwise:
        pts = [pts[0]] + pts[1:][::-1]
    segs = []
    for i in range(4):
        a, b = pts[i], pts[(i + 1) % 4]
        # the tangent at a point of a circle is perpendicular to its radius; the sense follows the travel direction
        ta = (b[0] - a[0], b[1] - a[1])
        c1 = (a[0] + KAPPA * (ta[0] if a[0] == 0 else 0), a[1] + KAPPA * (ta[1] if a[1] == 0 else 0))
        c2 = (b[0] + KAPPA * (-ta[0] if b[0] == 0 else 0), b[1] + KAPPA * (-ta[1] if b[1] == 0 else 0))
        segs.append((c1, c2, b))
    return pts[0], segs


def _contours(symbol):
    """[('poly', points) or ('circle', (start, segments))] in unit space: the disc counter-clockwise, then the cut-out
    as a hole (clockwise) and, for the outlined symbols, the island inside it (counter-clockwise)."""
    h = STROKE / 2
    out = [('circle', _circle_path(1.0, False))]
    if symbol == 'cross':
        out.append(('poly', _cross_polygon()[::-1]))
    elif symbol == 'circle':
        out += [('circle', _circle_path(RING + h, True)), ('circle', _circle_path(RING - h, False))]
    elif symbol == 'square':
        out += [('poly', _square(SQUARE + h)[::-1]), ('poly', _square(SQUARE - h))]
    elif symbol == 'triangle':
        out += [('poly', _triangle(TRIANGLE / 2 + h)[::-1]), ('poly', _triangle(TRIANGLE / 2 - h))]
    else:
        raise ValueError(symbol)
    return out


def _num(v):
    """A Type 2 charstring integer operand."""
    v = int(v)
    if -107 <= v <= 107:
        return bytes([v + 139])
    if 108 <= v <= 1131:
        v -= 108
        return bytes([(v >> 8) + 247, v & 0xff])
    if -1131 <= v <= -108:
        v = -v - 108
        return bytes([(v >> 8) + 251, v & 0xff])
    assert -32768 <= v <= 32767, v
    return bytes([28]) + (v & 0xffff).to_bytes(2, 'big')


RMOVETO, RLINETO, RRCURVETO, ENDCHAR = 21, 5, 8, 14


def charstring(symbol, cx, cy, radius, width_operand=None):
    """The Type 2 charstring of the disc with the symbol cut out, in font units (y up). width_operand: the advance
    minus the Private DICT's nominalWidthX, or None when the advance equals its defaultWidthX."""
    out = bytearray()
    cur = (0, 0)
    first = True

    def pt(p):
        return (round(cx + p[0] * radius), round(cy + p[1] * radius))

    def emit(args, op):
        nonlocal first
        if first and width_operand is not None:
            out.extend(_num(width_operand))
        first = False
        for a in args:
            out.extend(_num(a))
        out.append(op)

    def move(p):
        nonlocal cur
        p = pt(p)
        emit([p[0] - cur[0], p[1] - cur[1]], RMOVETO)
        cur = p

    for kind, data in _contours(symbol):
        if kind == 'poly':
            move(data[0])
            for q in data[1:]:
                q = pt(q)
                emit([q[0] - cur[0], q[1] - cur[1]], RLINETO)
                cur = q
        else:
            start, segs = data
            move(start)
            for c1, c2, end in segs:
                c1, c2, end = pt(c1), pt(c2), pt(end)
                emit([c1[0] - cur[0], c1[1] - cur[1], c2[0] - c1[0], c2[1] - c1[1], end[0] - c2[0], end[1] - c2[1]],
                     RRCURVETO)
                cur = end
    out.append(ENDCHAR)
    return bytes(out)
