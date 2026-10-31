"""Measure the dashboard artwork: dial centres/radii and telltale bounding boxes.

Run from the project root:  python tools/analyse_bg.py <background.png>
"""

import sys

import png

LIT = 26  # a channel above this counts as "not background black"


def is_lit(p):
    return p[0] > LIT or p[1] > LIT or p[2] > LIT


def fit_circle(points):
    """Algebraic least-squares circle fit (Kasa)."""
    n = len(points)
    sx = sy = sxx = syy = sxy = sxxx = syyy = sxyy = sxxy = 0.0
    for x, y in points:
        sx += x
        sy += y
        sxx += x * x
        syy += y * y
        sxy += x * y
        sxxx += x * x * x
        syyy += y * y * y
        sxyy += x * y * y
        sxxy += x * x * y

    c1 = sxx + syy
    a11, a12, b1 = 2 * (sx * sx - n * sxx), 2 * (sx * sy - n * sxy), sx * c1 - n * (sxxx + sxyy)
    a21, a22, b2 = 2 * (sx * sy - n * sxy), 2 * (sy * sy - n * syy), sy * c1 - n * (sxxy + syyy)

    det = a11 * a22 - a12 * a21
    cx = (b1 * a22 - b2 * a12) / det
    cy = (a11 * b2 - a21 * b1) / det
    r = (sum((x - cx) ** 2 + (y - cy) ** 2 for x, y in points) / n) ** 0.5
    return cx, cy, r


def trace_outer_arc(img, x_lo, x_hi, y_lo, y_hi, from_left):
    """Follow the outermost lit pixel per row - the dial's outer ring.

    Restricted to the outer flank of each dial, where the ring is the only
    feature, so numerals and inner circles cannot pollute the fit.
    """
    pts = []
    xs = range(x_lo, x_hi) if from_left else range(x_hi - 1, x_lo - 1, -1)
    for y in range(y_lo, y_hi):
        for x in xs:
            if is_lit(img.px(x, y)):
                pts.append((x, y))
                break
    return pts


def bbox(img, x_lo, y_lo, x_hi, y_hi):
    """Tight bounding box of lit pixels within a window."""
    minx, miny, maxx, maxy = 10 ** 6, 10 ** 6, -1, -1
    for y in range(y_lo, y_hi):
        for x in range(x_lo, x_hi):
            if is_lit(img.px(x, y)):
                minx = min(minx, x)
                miny = min(miny, y)
                maxx = max(maxx, x)
                maxy = max(maxy, y)
    if maxx < 0:
        return None
    return minx, miny, maxx - minx + 1, maxy - miny + 1


def columns_with_content(img, y_lo, y_hi, x_lo, x_hi):
    """Which columns in a band contain lit pixels - used to split icon rows."""
    cols = []
    for x in range(x_lo, x_hi):
        for y in range(y_lo, y_hi):
            if is_lit(img.px(x, y)):
                cols.append(x)
                break
    return cols


def split_runs(cols, gap=6):
    """Group adjacent columns into runs, one per icon."""
    runs = []
    if not cols:
        return runs
    start = prev = cols[0]
    for c in cols[1:]:
        if c - prev > gap:
            runs.append((start, prev))
            start = c
        prev = c
    runs.append((start, prev))
    return runs


def main():
    path = sys.argv[1]
    img = png.read(path)
    print('image: %dx%d' % (img.width, img.height))

    # Dial rings: trace only the outer flank of each dial.
    left = trace_outer_arc(img, 20, 340, 120, 620, from_left=True)
    lcx, lcy, lr = fit_circle(left)
    print('left dial : centre=(%.1f, %.1f) r=%.1f  [%d pts]' % (lcx, lcy, lr, len(left)))

    right = trace_outer_arc(img, 1260, 1590, 120, 620, from_left=False)
    rcx, rcy, rr = fit_circle(right)
    print('right dial: centre=(%.1f, %.1f) r=%.1f  [%d pts]' % (rcx, rcy, rr, len(right)))

    # Telltale rows.
    print('\n-- top indicator row (turn/headlight) --')
    for a, b in split_runs(columns_with_content(img, 80, 140, 620, 980)):
        print('   %s' % (bbox(img, a, 80, b + 1, 140),))

    print('\n-- bottom telltale row --')
    for a, b in split_runs(columns_with_content(img, 565, 640, 600, 1000)):
        print('   %s' % (bbox(img, a, 565, b + 1, 640),))

    # Regions the UI has to write into.
    print('\n-- fuel / temp strips --')
    print('   fuel bar rule :', bbox(img, 270, 505, 470, 530))
    print('   temp bar rule :', bbox(img, 1130, 505, 1330, 530))


if __name__ == '__main__':
    main()
