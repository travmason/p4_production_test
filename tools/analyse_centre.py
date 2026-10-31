"""Second pass: isolate the telltale icons and map the gaps in the centre column.

The dim outline that links the two dials bridges the telltale icons, so the
icons are picked out by brightness instead of mere presence.
"""

import sys

import png

BRIGHT = 90   # telltale glyphs are saturated; the linking outline is not


def bright(p):
    return p[0] > BRIGHT or p[1] > BRIGHT or p[2] > BRIGHT


def lit(p, t=26):
    return p[0] > t or p[1] > t or p[2] > t


def runs(cols, gap=8):
    out = []
    if not cols:
        return out
    start = prev = cols[0]
    for c in cols[1:]:
        if c - prev > gap:
            out.append((start, prev))
            start = c
        prev = c
    out.append((start, prev))
    return out


def bbox(img, x_lo, y_lo, x_hi, y_hi, pred):
    minx, miny, maxx, maxy = 10 ** 6, 10 ** 6, -1, -1
    for y in range(y_lo, y_hi):
        for x in range(x_lo, x_hi):
            if pred(img.px(x, y)):
                minx, miny = min(minx, x), min(miny, y)
                maxx, maxy = max(maxx, x), max(maxy, y)
    if maxx < 0:
        return None
    return minx, miny, maxx - minx + 1, maxy - miny + 1


def main():
    img = png.read(sys.argv[1])

    print('-- bottom telltale row (bright pixels only) --')
    cols = [x for x in range(600, 1010)
            if any(bright(img.px(x, y)) for y in range(570, 635))]
    for a, b in runs(cols):
        bb = bbox(img, a, 570, b + 1, 635, bright)
        print('   x=%4d y=%3d w=%3d h=%3d   centre=(%.0f, %.0f)'
              % (bb[0], bb[1], bb[2], bb[3], bb[0] + bb[2] / 2, bb[1] + bb[3] / 2))

    print('\n-- centre column: occupied row bands (x 660..940) --')
    band_open = None
    for y in range(140, 560):
        occupied = any(lit(img.px(x, y)) for x in range(660, 941))
        if occupied and band_open is None:
            band_open = y
        elif not occupied and band_open is not None:
            print('   rows %3d..%3d  (h=%d)' % (band_open, y - 1, y - band_open))
            band_open = None
    if band_open is not None:
        print('   rows %3d..end' % band_open)

    print('\n-- vertical rules flanking the stack --')
    for x in range(660, 941):
        n = sum(1 for y in range(250, 530) if lit(img.px(x, y)))
        if n > 200:
            print('   x=%d  (%d lit rows)' % (x, n))

    print('\n-- deg C / psi cluster boxes --')
    print('   left  :', bbox(img, 655, 160, 740, 245, lambda p: lit(p)))
    print('   right :', bbox(img, 865, 160, 950, 245, lambda p: lit(p)))


if __name__ == '__main__':
    main()
