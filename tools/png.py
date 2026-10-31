"""Minimal PNG reader - enough for the dashboard artwork, no dependencies.

Handles 8-bit truecolour (with or without alpha), non-interlaced, which is what
the export tools produce. Returns a flat bytearray of RGB triples.
"""

import struct
import zlib


class Image:
    def __init__(self, width, height, rgb):
        self.width = width
        self.height = height
        self.rgb = rgb  # bytearray, 3 bytes per pixel, row-major

    def px(self, x, y):
        i = (y * self.width + x) * 3
        return self.rgb[i], self.rgb[i + 1], self.rgb[i + 2]

    def set(self, x, y, r, g, b):
        i = (y * self.width + x) * 3
        self.rgb[i] = r
        self.rgb[i + 1] = g
        self.rgb[i + 2] = b

    def crop(self, x0, y0, w, h):
        out = bytearray(w * h * 3)
        for y in range(h):
            src = ((y0 + y) * self.width + x0) * 3
            dst = y * w * 3
            out[dst:dst + w * 3] = self.rgb[src:src + w * 3]
        return Image(w, h, out)


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def read(path):
    with open(path, 'rb') as f:
        data = f.read()

    assert data[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'

    pos = 8
    idat = bytearray()
    width = height = depth = colour = None

    while pos < len(data):
        (length,) = struct.unpack('>I', data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length  # length + type + data + crc

        if ctype == b'IHDR':
            width, height, depth, colour, _, _, interlace = struct.unpack('>IIBBBBB', body)
            assert depth == 8, 'only 8-bit channels supported, got %d' % depth
            assert colour in (2, 6), 'only truecolour supported, got type %d' % colour
            assert interlace == 0, 'interlaced PNG not supported'
        elif ctype == b'IDAT':
            idat += body
        elif ctype == b'IEND':
            break

    raw = zlib.decompress(bytes(idat))
    channels = 3 if colour == 2 else 4
    stride = width * channels

    rgb = bytearray(width * height * 3)
    prev = bytearray(stride)
    p = 0
    for y in range(height):
        ftype = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride

        if ftype == 1:      # Sub
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif ftype == 2:    # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:    # Average
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:    # Paeth
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                c = prev[i - channels] if i >= channels else 0
                line[i] = (line[i] + _paeth(a, prev[i], c)) & 0xFF
        elif ftype != 0:
            raise ValueError('bad filter type %d on row %d' % (ftype, y))

        dst = y * width * 3
        if channels == 3:
            rgb[dst:dst + stride] = line
        else:
            # Composite onto black; the artwork has no meaningful translucency.
            for x in range(width):
                s = x * 4
                a = line[s + 3]
                d = dst + x * 3
                if a == 255:
                    rgb[d] = line[s]
                    rgb[d + 1] = line[s + 1]
                    rgb[d + 2] = line[s + 2]
                else:
                    rgb[d] = (line[s] * a) // 255
                    rgb[d + 1] = (line[s + 1] * a) // 255
                    rgb[d + 2] = (line[s + 2] * a) // 255

        prev = line

    return Image(width, height, rgb)
