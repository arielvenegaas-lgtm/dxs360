#!/usr/bin/env python3
"""Generate image-decoder test fixtures using only the Python standard library.

Emits, into the directory given as argv[1] (default /tmp/imgfix):
  rgb8.png rgba8.png gray8.png pal8.png   + matching .argb files
  rgb24.bmp rgba32.bmp                     + matching .argb files

Each .argb file holds w*h little-endian uint32 pixels in 0xAARRGGBB order -
exactly what src/img.c is expected to produce. No PIL / third-party deps.
"""
import os
import struct
import sys
import zlib

W, H = 6, 4


def rgba(x, y):
    return ((x * 37 + y * 11) & 255,
            (x * 5 + y * 53) & 255,
            (x * 97 + y * 3) & 255,
            (255 if (x + y) % 2 else 200))


def gray(x, y):
    return (x * 13 + y * 29) & 255


def chunk(typ, data):
    return (struct.pack('>I', len(data)) + typ + data +
            struct.pack('>I', zlib.crc32(typ + data) & 0xffffffff))


def png(ctype, rows, palette=None, trns=None):
    sig = b'\x89PNG\r\n\x1a\n'
    ihdr = struct.pack('>IIBBBBB', W, H, 8, ctype, 0, 0, 0)
    out = sig + chunk(b'IHDR', ihdr)
    if palette is not None:
        out += chunk(b'PLTE', palette)
    if trns is not None:
        out += chunk(b'tRNS', trns)
    raw = b''.join(b'\x00' + r for r in rows)   # filter byte 0 per scanline
    out += chunk(b'IDAT', zlib.compress(raw, 9))
    out += chunk(b'IEND', b'')
    return out


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def filtered_png(ctype, channels, filters):
    """PNG whose scanlines use the given filter types (one per row, cycling)."""
    bpp = channels
    raw = []
    for y in range(H):
        if ctype == 2:
            row = bytes(v for x in range(W) for v in rgba(x, y)[:3])
        else:
            row = bytes(v for x in range(W) for v in rgba(x, y))
        raw.append(row)
    filtered = []
    for y in range(H):
        ft = filters[y % len(filters)]
        cur = raw[y]
        prev = raw[y - 1] if y else bytes(len(cur))
        out = bytearray()
        for x in range(len(cur)):
            a = cur[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if ft == 0:
                pred = 0
            elif ft == 1:
                pred = a
            elif ft == 2:
                pred = b
            elif ft == 3:
                pred = (a + b) >> 1
            else:
                pred = paeth(a, b, c)
            out.append((cur[x] - pred) & 255)
        filtered.append(bytes([ft]) + bytes(out))
    body = b''.join(filtered)
    sig = b'\x89PNG\r\n\x1a\n'
    ihdr = struct.pack('>IIBBBBB', W, H, 8, ctype, 0, 0, 0)
    return (sig + chunk(b'IHDR', ihdr) +
            chunk(b'IDAT', zlib.compress(body, 9)) + chunk(b'IEND', b''))


def argb_bytes(pixels):
    return b''.join(struct.pack('<I', (a << 24) | (r << 16) | (g << 8) | b)
                    for (r, g, b, a) in pixels)


def bmp24(pixels):
    rowbytes = (W * 3 + 3) & ~3
    body = bytearray()
    for y in range(H - 1, -1, -1):              # bottom-up
        row = bytearray()
        for x in range(W):
            r, g, b, _ = pixels[y * W + x]
            row += bytes([b, g, r])
        row += b'\x00' * (rowbytes - len(row))
        body += row
    off = 54
    hdr = b'BM' + struct.pack('<IHHI', off + len(body), 0, 0, off)
    dib = struct.pack('<IiiHHIIiiII', 40, W, H, 1, 24, 0, len(body),
                      2835, 2835, 0, 0)
    return hdr + dib + bytes(body)


def bmp32(pixels):
    body = bytearray()
    for y in range(H - 1, -1, -1):              # bottom-up
        for x in range(W):
            r, g, b, a = pixels[y * W + x]
            body += bytes([b, g, r, a])
    off = 54
    hdr = b'BM' + struct.pack('<IHHI', off + len(body), 0, 0, off)
    dib = struct.pack('<IiiHHIIiiII', 40, W, H, 1, 32, 0, len(body),
                      2835, 2835, 0, 0)
    return hdr + dib + bytes(body)


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '/tmp/imgfix'
    os.makedirs(d, exist_ok=True)
    px = [rgba(x, y) for y in range(H) for x in range(W)]
    px_opaque = [(r, g, b, 255) for (r, g, b, _) in px]

    files = {}

    rows_rgb = [bytes(v for x in range(W) for v in px[y * W + x][:3])
                for y in range(H)]
    files['rgb8.png'] = (png(2, rows_rgb), argb_bytes(px_opaque))

    rows_rgba = [bytes(v for x in range(W) for v in px[y * W + x])
                 for y in range(H)]
    files['rgba8.png'] = (png(6, rows_rgba), argb_bytes(px))

    g = [gray(x, y) for y in range(H) for x in range(W)]
    rows_gray = [bytes(g[y * W + x] for x in range(W)) for y in range(H)]
    files['gray8.png'] = (png(0, rows_gray),
                          argb_bytes([(v, v, v, 255) for v in g]))

    palette = bytes([i for i in range(256) for _ in range(1)] * 0)  # placeholder
    pal = bytearray()
    for i in range(256):
        pal += bytes([i, (i * 3) & 255, (i * 7) & 255])
    idx = [(x * 3 + y * 5) & 255 for y in range(H) for x in range(W)]
    rows_pal = [bytes(idx[y * W + x] for x in range(W)) for y in range(H)]
    files['pal8.png'] = (png(3, rows_pal, palette=bytes(pal)),
                         argb_bytes([(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2], 255)
                                     for i in idx]))

    # Same pixels but every scanline uses a different PNG filter (Sub/Up/Avg/
    # Paeth), exercising the decoder's unfiltering against the original.
    files['filt_rgb.png'] = (filtered_png(2, 3, [0, 1, 2, 3]),
                             argb_bytes(px_opaque))
    files['filt_paeth.png'] = (filtered_png(6, 4, [4, 4, 4, 4]),
                               argb_bytes(px))

    files['rgb24.bmp'] = (bmp24(px), argb_bytes(px_opaque))
    files['rgba32.bmp'] = (bmp32(px), argb_bytes(px))

    for name, (data, exp) in files.items():
        with open(os.path.join(d, name), 'wb') as f:
            f.write(data)
        with open(os.path.join(d, name + '.argb'), 'wb') as f:
            f.write(exp)

    with open(os.path.join(d, 'dims.txt'), 'w') as f:
        f.write(f'{W} {H}\n')
    print('wrote fixtures to', d)


if __name__ == '__main__':
    main()
