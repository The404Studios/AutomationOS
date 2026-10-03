"""Pure-python PNG decoder (8-bit, non-interlaced, colour types 0/2/4/6) + screenshot statistics.

QEMU's `screendump` writes exactly this subset. No third-party imports, so proofkit runs on a bare WSL box.
"""
import struct
import zlib


def decode(path):
    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, w = 8, b"", None
    while pos < len(d):
        ln, typ = struct.unpack(">I4s", d[pos:pos + 8])
        body = d[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b"IHDR":
            w, h, depth, ctype, _c, _f, inter = struct.unpack(">IIBBBBB", body)
            if depth != 8 or inter != 0 or ctype not in (0, 2, 4, 6):
                raise ValueError("unsupported PNG (depth=%d type=%d interlace=%d)" % (depth, ctype, inter))
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
    ch = {0: 1, 2: 3, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * ch
    rows, prev = [], bytearray(stride)
    p = 0
    for _ in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        if f == 1:
            for i in range(ch, stride):
                line[i] = (line[i] + line[i - ch]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                b = prev[i]
                c = prev[i - ch] if i >= ch else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        rows.append(bytes(line))
        prev = line
    return w, h, ch, rows


def pixel(rows, ch, x, y):
    r = rows[y]
    i = x * ch
    if ch >= 3:
        return r[i], r[i + 1], r[i + 2]
    return r[i], r[i], r[i]


def stats(path, step=4):
    """-> dict(width,height,distinct,mean_luma,fn(rgb,tol)->fraction)"""
    w, h, ch, rows = decode(path)
    colors, luma, n = {}, 0, 0
    for y in range(0, h, step):
        for x in range(0, w, step):
            c = pixel(rows, ch, x, y)
            colors[c] = colors.get(c, 0) + 1
            luma += (c[0] * 299 + c[1] * 587 + c[2] * 114) // 1000
            n += 1

    def fraction(rgb, tol):
        hit = 0
        for c, k in colors.items():
            if abs(c[0] - rgb[0]) <= tol and abs(c[1] - rgb[1]) <= tol and abs(c[2] - rgb[2]) <= tol:
                hit += k
        return hit / n if n else 0.0

    return {"width": w, "height": h, "distinct": len(colors), "mean_luma": luma / n if n else 0,
            "fraction": fraction, "samples": n}


def compare(path_a, path_b, step=4):
    """Mean absolute per-channel difference (0..255) between two same-size screenshots."""
    wa, ha, ca, ra = decode(path_a)
    wb, hb, cb, rb = decode(path_b)
    if (wa, ha) != (wb, hb):
        return 255.0
    tot = n = 0
    for y in range(0, ha, step):
        for x in range(0, wa, step):
            a, b = pixel(ra, ca, x, y), pixel(rb, cb, x, y)
            tot += abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])
            n += 3
    return tot / n if n else 255.0
