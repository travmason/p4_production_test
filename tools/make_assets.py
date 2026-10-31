"""Turn the dashboard artwork into embeddable assets.

Produces, from a single 1600x720 PNG:

  dash_bg.bin        the background, with every telltale dimmed to its unlit state
  tt_<name>.bin      each telltale at full brightness, to overlay when it lights
  dash_geometry.h    dial centres, sweep angles and anchor points, measured from
                     the artwork so the code cannot drift away from the picture

Pixel data is written in LVGL's in-memory order for the depth set by BPP below:
RGB888 is stored B,G,R, RGB565 as little-endian 16-bit. BPP must match LVGL's
CONFIG_LV_COLOR_DEPTH; dash_assets.c asserts that at compile time.

Usage:  python tools/make_assets.py assets/dash_bg_source.png
"""

import math
import os
import sys

import png

OUT_DIR = os.path.join('components', 'dash_assets', 'assets')
HDR = os.path.join('components', 'dash_assets', 'include', 'dash_geometry.h')

UNLIT = 0.10   # brightness retained by an unlit telltale
PAD = 3        # pixels of margin kept around each sprite

# Telltales the UI draws somewhere other than where the artwork prints them.
# These are erased from the background instead of dimmed, so no ghost is left
# behind in the row. The sprite is still produced, for use at the new location.
RELOCATED = {'snow'}

# Icons lifted out of the artwork for reuse on other screens. Unlike telltales
# these are part of the cluster face and stay in the background untouched; only
# a sprite copy is emitted.
EXTRA_ICONS = [
    ('fuel',    330,  410,  555, 615),
    ('coolant', 1190, 1270, 555, 615),
]

# Bytes per pixel: must match LVGL's colour depth. 2 (RGB565) keeps the DSI
# bridge's per-frame read off the PSRAM bandwidth cliff at 1600x720.
BPP = 2

# Telltale name -> (search window x0, x1, y0, y1) in the artwork.
TELLTALES = [
    ('turn_left',  650,  720,   85, 135),
    ('headlight',  760,  840,   85, 135),
    ('turn_right', 870,  940,   85, 135),
    ('snow',       625,  680,  575, 630),
    ('brake',      692,  752,  575, 630),
    ('warning',    765,  822,  575, 630),
    ('engine',     833,  895,  575, 630),
    ('battery',    908,  966,  575, 630),
]


def lit(p, t=26):
    return p[0] > t or p[1] > t or p[2] > t


def bright(p, t=90):
    return p[0] > t or p[1] > t or p[2] > t


def measure_dial(img, x_lo, x_hi, y_guess):
    """Locate a dial from the outer edges of its ring.

    Fitting a circle to a traced flank looks tidier but is biased: only ~120
    degrees of arc is visible on each dial's outer side, and the fit pulls the
    centre outwards by a couple of pixels, which is enough to see the sweep arc
    sitting off-centre. Measuring the ring's two horizontal edges gives the
    centre and radius directly.

    The ring is broken at the bottom of each dial, where the FUEL/TEMP box cuts
    into it, so the vertical centre comes from the top edge plus the radius
    rather than from a second pair of edges.
    """
    cy = y_guess
    cx = r = None
    for _ in range(3):
        row = [x for x in range(x_lo, x_hi) if lit(img.px(x, int(round(cy))))]
        if len(row) < 2:
            raise SystemExit('could not find the dial ring near y=%d' % cy)
        cx = (min(row) + max(row)) / 2.0
        r = (max(row) - min(row)) / 2.0

        col = [y for y in range(0, img.height) if lit(img.px(int(round(cx)), y))]
        if not col:
            raise SystemExit('could not find the top of the dial ring')
        cy = min(col) + r

    return cx, cy, r


TICK_MIN_BRIGHT = 150   # the dial's own ring is dim brown (~107) - skip it
TICK_STEPS = 3600       # tenth-of-a-degree resolution

# The annulus to look for ticks in. Three things overlap out here, and this band
# is the one place that sees the ticks and nothing else:
#
#   r < ~271   the numerals, which would bridge adjacent ticks
#   r 277..298 the tick marks
#   r 289..298 the tachometer's redline, which sits on top of the outermost part
#              of the 7 and 8 ticks and would otherwise both bridge them and,
#              once filtered out by colour, hide the 8 tick completely
#
# Sampling 277..288 stays inside the redline and outside the numerals.
TICK_R_LO = 277
TICK_R_HI = 289


def is_tick_pixel(p):
    """True for a tick mark, false for the redline.

    Ticks are amber (green sits around 0.64 of red); the redline is pure red
    (about 0.10). Belt and braces given the radius band already excludes it.
    """
    r, g, b = p
    if max(r, g, b) <= TICK_MIN_BRIGHT:
        return False
    return g >= r * 0.4


def tick_sweep(img, cx, cy, r):
    """Measure the angles of the first and last tick marks on a dial.

    The arc has to line up with the scale the artwork prints, so this locates
    the tick marks themselves rather than the ring they sit on. Two things make
    that fiddly, and both are handled here:

      - the dial's outer ring is a continuous circle covering the whole scale,
        so a plain brightness test finds a 270 degree band instead of ticks.
        The ring is dim brown; real ticks are near-saturated, hence the
        threshold.
      - the numerals reach outward far enough to bridge adjacent ticks, so the
        sampled annulus is kept outside them.

    Angles follow LVGL's convention: 0 is at 3 o'clock, increasing clockwise.
    """
    hit = [False] * TICK_STEPS
    for i in range(TICK_STEPS):
        a = math.radians(360.0 * i / TICK_STEPS)
        ca, sa = math.cos(a), math.sin(a)
        for ri in range(TICK_R_LO, TICK_R_HI):
            x = int(round(cx + ri * ca))
            y = int(round(cy + ri * sa))
            if 0 <= x < img.width and 0 <= y < img.height and is_tick_pixel(img.px(x, y)):
                hit[i] = True
                break

    if not any(hit):
        raise SystemExit('no tick marks found - check TICK_R_LO/TICK_R_HI')

    # Find the widest empty run: the gap at the bottom of the dial. Rotating to
    # just past it removes the 0/360 wrap, which would otherwise split the tick
    # that straddles it into two half-clusters.
    best_len, best_end = 0, 0
    i = 0
    while i < TICK_STEPS:
        if hit[i]:
            i += 1
            continue
        j = i
        while j < TICK_STEPS * 2 and not hit[j % TICK_STEPS]:
            j += 1
        if j - i > best_len:
            best_len, best_end = j - i, j % TICK_STEPS
        i = j if j > i else i + 1

    if best_len < TICK_STEPS // 20:
        raise SystemExit('could not find the gap in the dial scale')

    rot = [hit[(best_end + k) % TICK_STEPS] for k in range(TICK_STEPS)]

    clusters = []
    k = 0
    while k < TICK_STEPS:
        if not rot[k]:
            k += 1
            continue
        s = k
        while k < TICK_STEPS and rot[k]:
            k += 1
        clusters.append((s + k - 1) / 2.0)

    if len(clusters) < 2:
        raise SystemExit('found %d tick marks, need at least 2' % len(clusters))

    deg = 360.0 / TICK_STEPS
    angles = [c * deg for c in clusters]

    # Least squares through all the ticks rather than just the two ends: the
    # ticks are evenly spaced by construction, so fitting angle = a + k*spacing
    # averages out the centroid noise that otherwise lands the sweep on the
    # wrong side of a rounding boundary.
    n = len(angles)
    mean_k = (n - 1) / 2.0
    mean_a = sum(angles) / n
    num = sum((k - mean_k) * (a - mean_a) for k, a in enumerate(angles))
    den = sum((k - mean_k) ** 2 for k in range(n))
    spacing = num / den
    first = mean_a - spacing * mean_k

    start = (best_end * deg + first) % 360.0
    sweep = spacing * (n - 1)

    # The ticks are evenly spaced, so uneven gaps mean something in the annulus
    # was mistaken for a tick (or two ticks were merged). Fail loudly rather
    # than silently drawing an arc that disagrees with the printed scale.
    gaps = [angles[i + 1] - angles[i] for i in range(n - 1)]
    nominal = spacing
    worst = max(abs(g - nominal) for g in gaps)
    if worst > nominal * 0.15:
        raise SystemExit(
            'tick spacing is uneven (nominal %.1f deg, worst deviation %.1f deg, '
            '%d ticks at %s) - detection picked up something that is not a tick'
            % (nominal, worst, len(clusters),
               ', '.join('%.1f' % ((best_end * deg + c * deg) % 360.0) for c in clusters)))

    print('     %d ticks, %.1f deg apart' % (len(clusters), nominal))
    return start, sweep


def bbox(img, x0, x1, y0, y1, pred):
    minx, miny, maxx, maxy = 10 ** 6, 10 ** 6, -1, -1
    for y in range(y0, y1):
        for x in range(x0, x1):
            if pred(img.px(x, y)):
                minx, miny = min(minx, x), min(miny, y)
                maxx, maxy = max(maxx, x), max(maxy, y)
    if maxx < 0:
        return None
    return minx, miny, maxx - minx + 1, maxy - miny + 1


def find_rule(img, x0, x1, y0, y1):
    """The track under FUEL / TEMP: the widest bright horizontal run.

    It is warm amber rather than white, so this tests brightness, not whiteness.
    """
    best = None
    for y in range(y0, y1):
        xs = [x for x in range(x0, x1) if max(img.px(x, y)) > 120]
        if len(xs) > 100 and (best is None or len(xs) > best[2]):
            best = (min(xs), y, len(xs))
    if best is None:
        return None
    return best[0], best[1], best[2]


def write_pixels(path, img, x0, y0, w, h):
    """Write raw pixels in LVGL's in-memory order for the chosen depth.

    RGB888 is stored B,G,R; RGB565 is stored little-endian 16-bit.
    """
    if BPP == 3:
        buf = bytearray(w * h * 3)
        i = 0
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                r, g, b = img.px(x, y)
                buf[i] = b; buf[i + 1] = g; buf[i + 2] = r
                i += 3
    else:
        buf = bytearray(w * h * 2)
        i = 0
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                r, g, b = img.px(x, y)
                v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                buf[i] = v & 0xFF; buf[i + 1] = (v >> 8) & 0xFF
                i += 2
    with open(path, 'wb') as f:
        f.write(buf)
    return len(buf)


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else 'assets/dash_bg_source.png'
    img = png.read(src)
    assert (img.width, img.height) == (1600, 720), \
        'expected 1600x720 artwork, got %dx%d' % (img.width, img.height)

    os.makedirs(OUT_DIR, exist_ok=True)
    os.makedirs(os.path.dirname(HDR), exist_ok=True)

    lcx, lcy, lr = measure_dial(img, 0, 700, 375)
    rcx, rcy, rr = measure_dial(img, 900, 1600, 375)
    l_start, l_sweep = tick_sweep(img, lcx, lcy, lr)
    r_start, r_sweep = tick_sweep(img, rcx, rcy, rr)

    # The two dials are the same drawing offset horizontally, so their centres
    # must mirror about the middle of the screen. A drift here means one of them
    # was measured against something other than its ring.
    if abs((lcx + rcx) - img.width) > 2.0:
        raise SystemExit('dial centres are not symmetric: %.1f + %.1f != %d'
                         % (lcx, rcx, img.width))

    print('left  dial: centre=(%.1f,%.1f) r=%.1f  start=%.1f deg sweep=%.1f deg'
          % (lcx, lcy, lr, l_start, l_sweep))
    print('right dial: centre=(%.1f,%.1f) r=%.1f  start=%.1f deg sweep=%.1f deg'
          % (rcx, rcy, rr, r_start, r_sweep))

    # Locate every telltale before dimming, so sprites keep full brightness.
    sprites = []
    icons = []
    for name, x0, x1, y0, y1 in TELLTALES:
        bb = bbox(img, x0, x1, y0, y1, bright)
        if bb is None:
            raise SystemExit('telltale %s not found in its search window' % name)
        bx = max(0, bb[0] - PAD)
        by = max(0, bb[1] - PAD)
        bw = min(img.width - bx, bb[2] + PAD * 2)
        bh = min(img.height - by, bb[3] + PAD * 2)
        sprites.append((name, bx, by, bw, bh))
        n = write_pixels(os.path.join(OUT_DIR, 'tt_%s.bin' % name), img, bx, by, bw, bh)
        print('  sprite %-11s x=%4d y=%3d %3dx%-3d (%d B)' % (name, bx, by, bw, bh, n))

    for name, x0, x1, y0, y1 in EXTRA_ICONS:
        bb = bbox(img, x0, x1, y0, y1, lambda p: bright(p, 60))
        if bb is None:
            raise SystemExit('icon %s not found' % name)
        ix, iy = max(0, bb[0] - 2), max(0, bb[1] - 2)
        iw, ih = bb[2] + 4, bb[3] + 4
        n = write_pixels(os.path.join(OUT_DIR, 'ic_%s.bin' % name), img, ix, iy, iw, ih)
        icons.append((name, iw, ih))
        print('  icon   %-11s %3dx%-3d (%d B)' % (name, iw, ih, n))

    fuel = find_rule(img, 250, 500, 505, 535)
    temp = find_rule(img, 1100, 1360, 505, 535)
    if fuel is None or temp is None:
        raise SystemExit('could not locate the fuel/temp tracks')

    # Both centre gauges now read oil, so the left icon becomes a second oil can.
    # Doing it here rather than as a runtime overlay costs nothing to draw: it is
    # just part of the background blit that happens anyway.
    left_icon = bbox(img, 655, 720, 193, 245, lit)
    oil_icon = bbox(img, 875, 935, 193, 245, lit)
    if left_icon is None or oil_icon is None:
        raise SystemExit('could not locate the centre icons')

    lx, ly, lw, lh = left_icon
    ox, oy, ow, oh = oil_icon
    for y in range(ly, ly + lh):                    # erase the thermometer
        for x in range(lx, lx + lw):
            img.set(x, y, 0, 0, 0)
    dx = lx + lw // 2 - ow // 2                     # centre the can where it was
    dy = ly + lh // 2 - oh // 2
    for y in range(oh):
        for x in range(ow):
            r, g, b = img.px(ox + x, oy + y)
            img.set(dx + x, dy + y, r, g, b)
    print('  left icon %s -> oil can copied from %s at (%d,%d)'
          % (left_icon, oil_icon, dx, dy))
    print('  fuel track: x=%d y=%d w=%d' % fuel)
    print('  temp track: x=%d y=%d w=%d' % temp)

    # Dim the telltales in place - or erase the relocated ones - then emit the
    # background.
    for name, bx, by, bw, bh in sprites:
        scale = 0.0 if name in RELOCATED else UNLIT
        for y in range(by, by + bh):
            for x in range(bx, bx + bw):
                r, g, b = img.px(x, y)
                img.set(x, y, int(r * scale), int(g * scale), int(b * scale))
        if scale == 0.0:
            print('  %s erased from the background (drawn elsewhere)' % name)

    n = write_pixels(os.path.join(OUT_DIR, 'dash_bg.bin'), img, 0, 0, img.width, img.height)
    print('  background: %d bytes' % n)

    with open(HDR, 'w') as f:
        f.write('/* Generated by tools/make_assets.py - do not edit by hand. */\n')
        f.write('#pragma once\n\n')
        f.write('#define DASH_BG_W %d\n#define DASH_BG_H %d\n#define DASH_BPP %d\n\n' % (img.width, img.height, BPP))

        for tag, cx, cy, r, st, sw in (('L', lcx, lcy, lr, l_start, l_sweep),
                                       ('R', rcx, rcy, rr, r_start, r_sweep)):
            # LVGL takes whole degrees. Round the two ends independently rather
            # than rounding the sweep, so the error is split between the first
            # and last tick instead of accumulating at the top of the scale.
            start_i = round(st)
            sweep_i = round(st + sw) - start_i
            f.write('#define DIAL_%s_CX %d\n' % (tag, round(cx)))
            f.write('#define DIAL_%s_CY %d\n' % (tag, round(cy)))
            f.write('#define DIAL_%s_R %d\n' % (tag, round(r)))
            f.write('#define DIAL_%s_START %d\n' % (tag, start_i))
            f.write('#define DIAL_%s_SWEEP %d\n\n' % (tag, sweep_i))

        for name, bx, by, bw, bh in sprites:
            u = name.upper()
            f.write('#define TT_%s_X %d\n#define TT_%s_Y %d\n' % (u, bx, u, by))
            f.write('#define TT_%s_W %d\n#define TT_%s_H %d\n\n' % (u, bw, u, bh))

        for name, iw, ih in icons:
            u = name.upper()
            f.write('#define IC_%s_W %d\n#define IC_%s_H %d\n\n' % (u, iw, u, ih))

        f.write('#define FUEL_TRACK_X %d\n#define FUEL_TRACK_Y %d\n#define FUEL_TRACK_W %d\n\n'
                % fuel)
        f.write('#define TEMP_TRACK_X %d\n#define TEMP_TRACK_Y %d\n#define TEMP_TRACK_W %d\n\n'
                % temp)

        # Anchors for the values the artwork leaves room for (centre points).
        f.write('/* Centre points of the gaps the artwork leaves for live values. */\n')
        for name, x, y in (('TRIP', 799, 235), ('TOTAL', 799, 347), ('TURBO', 799, 473),
                           ('COOLANT', 690, 258), ('OIL', 905, 258)):
            f.write('#define VAL_%s_CX %d\n#define VAL_%s_CY %d\n' % (name, x, name, y))

    print('wrote %s' % HDR)


if __name__ == '__main__':
    main()
