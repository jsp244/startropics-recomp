"""How two of the self-test's pictures differ: the number of pixels and the largest difference of
one colour channel (0-255). Reads the 8-bit RGB / RGBA PNGs runtime/png.c writes, with the
standard library only.

usage: python3 tools/macos/pngdiff.py a.png b.png
"""
import struct
import sys
import zlib


def load(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    pos, idat, width, height, kind = 8, b"", 0, 0, 2
    while pos < len(data):
        n, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if tag == b"IHDR":
            width, height, depth, kind = struct.unpack(">IIBB", body[:10])
            if depth != 8 or kind not in (2, 6):
                raise SystemExit(f"{path}: only 8-bit RGB / RGBA")
        elif tag == b"IDAT":
            idat += body
    bpp = 3 if kind == 2 else 4
    raw, stride = zlib.decompress(idat), width * bpp
    out, prev, i = bytearray(), bytearray(stride), 0
    for _ in range(height):
        f, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line
        prev = line
    return width, height, bpp, bytes(out)


def main():
    wa, ha, bpp, a = load(sys.argv[1])
    wb, hb, bppb, b = load(sys.argv[2])
    if (wa, ha, bpp) != (wb, hb, bppb):
        raise SystemExit(f"different sizes: {wa}x{ha} and {wb}x{hb}")
    pixels, worst = 0, 0
    for p in range(0, len(a), bpp):
        d = max(abs(a[p + k] - b[p + k]) for k in range(bpp))
        if d:
            pixels += 1
            worst = max(worst, d)
    print(f"{pixels} of {wa * ha} pixels differ, by at most {worst} in a colour channel")
    sys.exit(0 if worst <= 1 else 1)


main()
