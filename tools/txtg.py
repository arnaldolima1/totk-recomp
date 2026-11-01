"""A reader for the TexToGo (.txtg) textures of Tears of the Kingdom: one mip level of a texture, deswizzled, as RGBA8
(BC1, BC3, BC4, BC5, R8, RG8, RGBA8) or as linear ASTC blocks for the runtime's decoder (tex_astc.c).

The container (facts read from BladesawStudios/TxtgSharp, AGPL, for behaviour only; this code is our own): a 0x50-byte
header, version 0x11 at +2, '6PK0' at +4, u16 width +8, height +10, layers +12, u8 mips +14, u16 format code +0x3C, u32
+0x44 whose low byte holds an ASTC footprint ((w-1) << 4 | (h-1)) when its upper bits are 0x7F00. Then one u32 per
surface (layer in the low 16 bits, mip in bits 16..23), then per surface an i32 compressed size and a u32 flag, then
the surfaces, each one zstd frame of the Tegra X1 block-linear image.

    python tools/txtg.py <file.txtg> <out.png> [max side]
"""
import struct
import sys

try:   # Python 3.14 (the release's own Python): the standard library's zstd; else the zstandard package
    from compression import zstd as _zstd

    def unzstd(frame, zdict=None):
        return _zstd.decompress(frame, zstd_dict=zdict)

    def zstd_dict(data):
        return _zstd.ZstdDict(data)
except ImportError:
    import zstandard as _zstd

    def unzstd(frame, zdict=None):
        d = _zstd.ZstdDecompressor(dict_data=zdict) if zdict is not None else _zstd.ZstdDecompressor()
        return d.stream_reader(frame).read()

    def zstd_dict(data):
        return _zstd.ZstdCompressionDict(data)

# Format code -> (kind, block width, block height, bytes per block)
def format_of(code, setting2):
    hi = code >> 8
    if code in (0x101, 0x102, 0x105, 0x106, 0x107, 0x109, 0x10A, 0x10C):
        bw, bh = (4, 4) if code == 0x109 else (8, 5) if code == 0x101 else (8, 8)
        if (setting2 & 0xFFFFFF00) == 0x7F00:
            bw, bh = ((setting2 >> 4) & 0xF) + 1, (setting2 & 0xF) + 1
        return 'astc', bw, bh, 16
    if code == 0xA0A:
        return 'rgba8', 1, 1, 4
    if code == 0xB0B:
        return 'rg8', 1, 1, 2
    if code == 0xC0C:
        return 'r8', 1, 1, 1
    return {2: ('bc1', 4, 4, 8), 3: ('bc1', 4, 4, 8), 5: ('bc3', 4, 4, 16), 6: ('bc4', 4, 4, 8), 7: ('bc5', 4, 4, 16),
            9: ('bc7', 4, 4, 16)}.get(hi, ('unknown', 4, 4, 16))


def _block_height_mip0(h_blocks):
    x = h_blocks + h_blocks // 2
    return 16 if x >= 128 else 8 if x >= 64 else 4 if x >= 32 else 2 if x >= 16 else 1


def deswizzle(data, w_blocks, h_blocks, bpb, base_h_blocks):
    """Tegra X1 block-linear to linear rows of blocks. A GOB is 64 bytes x 8 rows (512 bytes); GOBs stack block_height
    high, and such columns run across the row; the block height comes from the top mip's height."""
    row = w_blocks * bpb
    gob_h = min(_block_height_mip0(base_h_blocks), max(1, min(16, 1 << max(0, ((h_blocks + 7) // 8 - 1).bit_length()))))
    gobs_x = (row + 63) // 64
    col_bytes = 512 * gob_h
    out = bytearray(row * h_blocks)
    for y in range(h_blocks):
        base = (y // (8 * gob_h)) * col_bytes * gobs_x + ((y % (8 * gob_h)) // 8) * 512 + ((y % 8) >> 1) * 64 + (y & 1) * 16
        for xb in range(0, row, 16):
            src = base + (xb // 64) * col_bytes + ((xb % 64) // 32) * 256 + ((xb % 32) // 16) * 32
            n = min(16, row - xb)
            out[y * row + xb:y * row + xb + n] = data[src:src + n]
    return bytes(out)


def _c565(c):
    return ((c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31)


def _bc1_block(b, alpha_ok=True):
    c0, c1, bits = struct.unpack_from('<HHI', b)
    a, bb = _c565(c0), _c565(c1)
    if c0 > c1 or not alpha_ok:
        pal = [a + (255,), bb + (255,), tuple((2 * a[i] + bb[i]) // 3 for i in range(3)) + (255,),
               tuple((a[i] + 2 * bb[i]) // 3 for i in range(3)) + (255,)]
    else:
        pal = [a + (255,), bb + (255,), tuple((a[i] + bb[i]) // 2 for i in range(3)) + (255,), (0, 0, 0, 0)]
    return [pal[(bits >> (2 * i)) & 3] for i in range(16)]


def _bc4_block(b):
    a0, a1 = b[0], b[1]
    bits = int.from_bytes(b[2:8], 'little')
    if a0 > a1:
        pal = [a0, a1] + [((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)]
    else:
        pal = [a0, a1] + [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255]
    return [pal[(bits >> (3 * i)) & 7] for i in range(16)]


def decode(kind, lin, w, h, bw, bh, bpb):
    """RGBA8 rows of a decoded image, or None for ASTC (the runtime decodes it) and unsupported kinds."""
    wb, hb = (w + bw - 1) // bw, (h + bh - 1) // bh
    out = bytearray(w * h * 4)
    if kind in ('rgba8', 'rg8', 'r8'):
        for y in range(h):
            for x in range(w):
                p = lin[(y * w + x) * bpb:(y * w + x + 1) * bpb]
                px = (p[0], p[1], p[2], p[3]) if kind == 'rgba8' else (p[0], p[1], 0, 255) if kind == 'rg8' else (p[0],) * 3 + (255,)
                out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes(px)
        return bytes(out)
    if kind not in ('bc1', 'bc3', 'bc4', 'bc5'):
        return None
    for by in range(hb):
        for bx in range(wb):
            b = lin[(by * wb + bx) * bpb:(by * wb + bx + 1) * bpb]
            if kind == 'bc1':
                px = _bc1_block(b)
            elif kind == 'bc3':
                al = _bc4_block(b[0:8])
                px = [c[:3] + (al[i],) for i, c in enumerate(_bc1_block(b[8:16], False))]
            elif kind == 'bc4':
                px = [(v, v, v, 255) for v in _bc4_block(b)]
            else:
                r, g = _bc4_block(b[0:8]), _bc4_block(b[8:16])
                px = [(r[i], g[i], 255, 255) for i in range(16)]
            for i in range(16):
                x, y = bx * 4 + (i & 3), by * 4 + (i >> 2)
                if x < w and y < h:
                    out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes(px[i])
    return bytes(out)


def read_txtg(data, max_side=512):
    """(width, height, kind, bw, bh, payload): the largest mip of layer 0 no bigger than max_side; payload is RGBA8 rows,
    or linear ASTC blocks when kind is 'astc'."""
    if data[4:8] != b'6PK0' or struct.unpack_from('<H', data, 2)[0] != 0x11:
        raise ValueError('not a TexToGo file')
    w, h, layers = struct.unpack_from('<HHH', data, 8)
    mips = data[14]
    code = struct.unpack_from('<H', data, 0x3C)[0]
    s2 = struct.unpack_from('<I', data, 0x44)[0]
    kind, bw, bh, bpb = format_of(code, s2)
    n = layers * mips
    entries = struct.unpack_from(f'<{n}I', data, 0x50)
    p = 0x50 + 4 * n
    sizes = [struct.unpack_from('<iI', data, p + 8 * i)[0] for i in range(n)]
    p += 8 * n
    want = 0
    while want + 1 < mips and max(w >> want, h >> want) > max_side:
        want += 1
    base_hb = (h + bh - 1) // bh
    for i in range(n):
        frame = data[p:p + sizes[i]]
        p += sizes[i]
        layer, mip = entries[i] & 0xFFFF, (entries[i] >> 16) & 0xFF
        if layer or mip != want:
            continue
        mw, mh = max(1, w >> mip), max(1, h >> mip)
        raw = unzstd(frame)
        wb, hb = (mw + bw - 1) // bw, (mh + bh - 1) // bh
        lin = deswizzle(raw, wb, hb, bpb, base_hb)
        if kind == 'astc':
            return mw, mh, kind, bw, bh, lin
        return mw, mh, kind, bw, bh, decode(kind, lin, mw, mh, bw, bh, bpb)
    raise ValueError('mip not found')


def main(argv):
    from PIL import Image
    data = open(argv[1], 'rb').read()
    w, h, kind, bw, bh, px = read_txtg(data, int(argv[3]) if len(argv) > 3 else 512)
    print(w, h, kind, bw, bh)
    if px is not None and kind != 'astc':
        Image.frombytes('RGBA', (w, h), px).save(argv[2])


if __name__ == '__main__':
    main(sys.argv)
