# Adapted from MacNativeGames games/totk/scripts/make_romfs_patches.py @ ebe621d (MIT, Copyright (c) 2026
# MacNativeGames contributors).
"""Build the 60 fps RomFS overlay for TotK 1.1.0, 1.2.1 or 1.4.3 (runtime/romfs.c, TOTK_ROMFS_PATCHES).

UI/LayoutArchive/Common.Product.<110 or 140>.Nin_NX_NVN.blarc.zs: the weapon / shield / material quick menus capture the game
picture for their background when the panes C_CaptureBG_00, C_CaptureClearBG_00 and P_CaptureClearBG_00 become
visible at animation frame 1.0 (anim/ShortCutBG_00_In.bflan), AutoBuilder the same with C_CaptureBG_00
(anim/AutoBuilder_00_In.bflan). UI animations advance 0.5 frames per game frame at 60 fps, so the capture lands after
the world stopped drawing and the background can be black. The visibility keys move to frame 0.0: the live world is
captured at once (upstream checked weapon, shield, material and ability wheel at 60 fps, and 30 fps).
The game-over flow fix of the same upstream script is 1.0.0 only (1.1.0, 1.2.1 and 1.4.3 run DmF_SY_GameOver.110) and is
left out.

--aspect <w>x<h> (a game resolution of another shape than 16:9, such as 3440x1440 or 1280x800) also lays the UI out
for that shape (tools/ultrawide_layout.py): menus keep their 16:9 proportions in the middle, full-screen backgrounds
cover the screen and the HUD moves to the screen's corners (wider) or its top and bottom edges (narrower). totk.exe
loads that overlay from romfs-patches-<w>x<h> when the game resolution is that size (runtime/romfs.c overlay_init);
the camera's side of it is the patch set aspect (recomp/patches-v121.txt, patches-v143.txt).

--prompts xbox | xbox-label: the same overlay with Xbox button icons in the game's prompts, for prompts = xbox
(runtime/romfs.c overlay_init loads romfs-patches-xbox or romfs-patches-xbox-label in place of romfs-patches). Drawn
from the game's own files on the player's machine, nothing added (tools/button_icons.py, which needs fontTools):
- both: L, R, ZL, ZR, Plus, Minus and the d-pad become LB, RB, LT, RT, Menu, View and a cross in the button font
  (scft/External.bfotf, the same font in each Font/Font*.Nin_NX_NVN.bfarc.zs), and L and ZL in the button guide's images
  (Nt_KeyTexL_00 and Nt_KeyTexZL_00 of timg/__Combined.bntx in the Common archive). The fonts grow, so the overlay also
  carries the resource size table (System/Resource/ResourceSizeTable.Product.<n>...rsizetable.zs) with their entries
  raised by as much.
- xbox, for pad_layout = nintendo. That layout plays an Xbox pad by position: the button that does Switch A is
  labelled B, so the face buttons' prompts are swapped to name the labels, A with B and X with Y:
  - the action prompt (blyt/DoCommand_00.bflyt) draws the letter images Nt_KeyTexA/B/X/Y_00 of timg/__Combined.bntx in
    the Common archive: their image data is swapped (same format, size and tiling). Around it, a diamond names the
    other face buttons with one-letter text panes (T_B_00, T_X_00, ...). The game fills them from the messages of the
    same name in LayoutMsg/DoCommand_00.msbt of each Mals/<language> archive, over the layout's own text: both have
    their letters swapped the same way. The diamond's shape (Nt_KeyBtnSubBase*) shows the button's position, which
    stays right.
  - every other prompt: the glyphs U+E040 A, U+E041 B, U+E042 X, U+E043 Y of the button font trade places.
- xbox-label, for pad_layout = xbox and auto. Those layouts put Switch A on the button labelled A (auto picks xbox for
  every pad not labelled the Nintendo way), so the face buttons keep their letters; the action prompt's diamond keeps
  the Switch positions (A on the right).
--xbox-prompts is the older name of --prompts xbox.

--prompts playstation: the same overlay with PlayStation symbols in place of the letters, for prompts = playstation
(romfs-patches-ps). That setting goes with the buttons played by label (pad_layout = xbox or auto) (cross is Switch A), so A becomes the cross, B
the circle, X the square and Y the triangle. The symbols are drawn by tools/ps_prompts.py, not taken from anywhere:
- the letter images Nt_KeyTexA/B/X/Y_00 (R8 alpha masks of a white disc with the letter cut out) are drawn again as
  the same disc, measured from the game's A image, with the symbol cut out;
- the glyphs U+E040 to U+E043 of scft/External.bfotf get outlines of the same disc and symbol (bfotf_symbols);
- the diamond's one-letter text panes and messages get the characters U+00D7, U+25CB, U+25A1 and U+25B3, which the
  game's text font (Normal_00, the Rodin fonts) draws.

  --playstation-prompts is the older name of --prompts playstation. The other prompt files are left as they are.

Input: build/extracted/<version>/romfs (romfs.bin and index.json, verified by tools/extract.py). Output:
build/<version>/romfs-patches/<path> by default (romfs-patches-<style> with --prompts), and <out>/manifest.tsv (path, size, SHA-256 of the overlay, SHA-256 of the original). Run with .venv-py314.
"""
import argparse
import compression.zstd as zstd
import hashlib
import importlib.util
import json
import re
import struct
from pathlib import Path

root = Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
ap.add_argument('--version', choices=('v110', 'v121', 'v143'), default='v121')
ap.add_argument('--romfs', type=Path, help='default build/extracted/<version>/romfs')
ap.add_argument('--out', type=Path,
                help='default build/<version>/romfs-patches (romfs-patches-<style> with --prompts)')
ap.add_argument('--aspect', help='<w>x<h> of a game resolution of another shape than 16:9: also lay the UI out for it')
ap.add_argument('--prompts', choices=('xbox', 'xbox-label', 'playstation'),
                help='Xbox button icons in the prompts: xbox for pad_layout = nintendo (A shown as B, X as Y), '
                     'xbox-label for pad_layout = xbox or auto (the face buttons keep their letters); playstation: '
                     'PlayStation symbols on the face buttons, for pad_layout = xbox or auto')
ap.add_argument('--xbox-prompts', dest='prompts', action='store_const', const='xbox', help='the same as --prompts xbox')
ap.add_argument('--playstation-prompts', dest='prompts', action='store_const', const='playstation',
                help='the same as --prompts playstation')
args = ap.parse_args()
args.romfs = args.romfs or root / 'build/extracted' / args.version / 'romfs'
OUT_NAME = {None: 'romfs-patches', 'playstation': 'romfs-patches-ps'}.get(args.prompts, f'romfs-patches-{args.prompts}')
args.out = args.out or root / 'build' / args.version / OUT_NAME
index = json.loads((args.romfs / 'index.json').read_text())
assert index['status'] == 'pass'
files = {f['path']: f for f in index['files']}


def sarc_nodes(data):
    """(name, node offset, absolute start, end) of every SARC member, in SFAT order."""
    assert data[:4] == b'SARC' and data[6:8] == b'\xff\xfe'
    header, size, start = struct.unpack_from('<HxxII', data, 4)
    assert size == len(data) and data[header:header + 4] == b'SFAT'
    count = struct.unpack_from('<H', data, header + 6)[0]
    names = header + 12 + count * 16
    assert data[names:names + 4] == b'SFNT'
    names += 8
    out = []
    for i in range(count):
        node = header + 12 + i * 16
        _, attribute, lo, hi = struct.unpack_from('<4I', data, node)
        offset = names + (attribute & 0xffffff) * 4
        out.append((data[offset:data.index(0, offset, start)].decode(), node, start + lo, start + hi))
    return out


def sarc_members(data):
    """name -> (absolute start, end) of every SARC member."""
    return {name: (lo, hi) for name, _, lo, hi in sarc_nodes(data)}


def flvi_keys(data, base):
    """(pane name, absolute key offset, keys) of every FLVI (visibility) step track in the BFLAN at data[base:]."""
    assert data[base:base + 4] == b'FLAN' and data[base + 4:base + 6] == b'\xff\xfe'
    pos, out = base + 0x14, []
    for _ in range(struct.unpack_from('<H', data, base + 0x10)[0]):
        magic, size = data[pos:pos + 4], struct.unpack_from('<I', data, pos + 4)[0]
        if magic == b'pai1':
            count, table = struct.unpack_from('<H', data, pos + 14)[0], pos + struct.unpack_from('<I', data, pos + 16)[0]
            for e in range(count):
                eo = pos + struct.unpack_from('<I', data, table + 4 * e)[0]
                name = data[eo:eo + 28].split(b'\0')[0].decode()
                for t in range(data[eo + 28]):
                    to = eo + struct.unpack_from('<I', data, eo + 32 + 4 * t)[0]
                    if data[to:to + 4] != b'FLVI':
                        continue
                    for g in range(data[to + 4]):
                        go = to + struct.unpack_from('<I', data, to + 8 + 4 * g)[0]
                        assert data[go + 2] == 1, 'visibility tracks are step curves'
                        ko = go + struct.unpack_from('<I', data, go + 8)[0]
                        keys = [struct.unpack_from('<fH', data, ko + 8 * k)
                                for k in range(struct.unpack_from('<H', data, go + 4)[0])]
                        out.append((name, ko, keys))
        pos += size
    return out


def bntx_swap(data, base, pairs):
    """Swap the image data of each (name, name) pair of textures in the BNTX at data[base:] (names without the ^
    suffix). Both of a pair must have the same header from flags to layout (format, size, tiling, mips) and image
    size, so the bytes of one are a valid image of the other."""
    assert data[base:base + 4] == b'BNTX' and data[base + 0x20:base + 0x24] == b'NX  '
    count, table = struct.unpack_from('<IQ', data, base + 0x24)
    found = {}
    for k in range(count):
        info = base + struct.unpack_from('<Q', data, base + table + 8 * k)[0]
        assert data[info:info + 4] == b'BRTI'
        name = base + struct.unpack_from('<Q', data, info + 0x60)[0]
        name = data[name + 2:name + 2 + struct.unpack_from('<H', data, name)[0]].decode().split('^')[0]
        mips = base + struct.unpack_from('<Q', data, info + 0x70)[0]
        shape = bytes(data[info + 0x10:info + 0x3c]) + bytes(data[info + 0x50:info + 0x54])
        start, size = base + struct.unpack_from('<Q', data, mips)[0], struct.unpack_from('<I', data, info + 0x50)[0]
        found[name] = (shape, start, size)
    for a, b in pairs:
        assert a in found and b in found, f'{a} or {b} missing from the texture list'
        (shape_a, at_a, size), (shape_b, at_b, _) = found[a], found[b]
        assert shape_a == shape_b, f'{a} and {b} differ in format, size or tiling'
        data[at_a:at_a + size], data[at_b:at_b + size] = data[at_b:at_b + size], data[at_a:at_a + size]


def bflyt_letters(data, base, end, swap):
    """Swap the text of the one-letter text panes T_<letter>_<n> (A, B, X or Y) in the BFLYT at data[base:end] by
    swap (letter -> letter); the names of the panes changed. Same length (one UTF-16 letter and its terminator)."""
    assert data[base:base + 4] == b'FLYT'
    pos, changed = base + struct.unpack_from('<H', data, base + 6)[0], []
    while pos + 8 <= end:
        magic, size = data[pos:pos + 4], struct.unpack_from('<I', data, pos + 4)[0]
        if magic == b'txt1':
            name = data[pos + 12:pos + 36].split(b'\0')[0].decode()
            length = struct.unpack_from('<H', data, pos + 0x56)[0]
            at = pos + struct.unpack_from('<I', data, pos + 0x64)[0]
            letter = data[at:at + 2].decode('utf-16-le') if length == 4 and at > pos else ''
            if letter in swap and re.fullmatch(r'T_[ABXY]_\d+', name):
                data[at:at + 2] = swap[letter].encode('utf-16-le')
                changed.append(name)
        if not size:
            break
        pos += size
    return changed


def msbt_letters(data, swap):
    """Swap the one-letter messages labelled T_<letter>_<n> (A, B, X or Y) in the UTF-16 MSBT data (a bytearray) by
    swap; the labels changed. The game sets a layout's text panes from the messages of the same name in
    Mals/<language>/LayoutMsg/<layout>.msbt, over the text stored in the layout."""
    assert data[:8] == b'MsgStdBn' and data[0x0c] == 1, 'not a UTF-16 MSBT'
    sections, pos = {}, 0x20
    for _ in range(struct.unpack_from('<H', data, 0x0e)[0]):
        size = struct.unpack_from('<I', data, pos + 4)[0]
        sections[bytes(data[pos:pos + 4])] = (pos + 16, size)
        pos = (pos + 16 + size + 15) & ~15
    labels, (base, _) = {}, sections[b'LBL1']
    for g in range(struct.unpack_from('<I', data, base)[0]):
        count, at = struct.unpack_from('<II', data, base + 4 + 8 * g)
        at += base
        for _ in range(count):
            length = data[at]
            labels[struct.unpack_from('<I', data, at + 1 + length)[0]] = data[at + 1:at + 1 + length].decode()
            at += 1 + length + 4
    (base, size), changed = sections[b'TXT2'], []
    count = struct.unpack_from('<I', data, base)[0]
    offsets = list(struct.unpack_from(f'<{count}I', data, base + 4)) + [size]
    for i in range(count):
        at, length = base + offsets[i], offsets[i + 1] - offsets[i]
        letter = data[at:at + 2].decode('utf-16-le') if length == 4 else ''
        if letter in swap and re.fullmatch(r'T_[ABXY]_\d+', labels.get(i, '')):
            data[at:at + 2] = swap[letter].encode('utf-16-le')
            changed.append(labels[i])
    return changed


def cff_index(data, pos):
    """([(start, end)] of every item, offSize, end) of the CFF INDEX at data[pos:]."""
    count = struct.unpack_from('>H', data, pos)[0]
    if not count:
        return [], 0, pos + 2
    size = data[pos + 2]
    offsets = [int.from_bytes(data[pos + 3 + i * size:pos + 3 + (i + 1) * size], 'big') for i in range(count + 1)]
    first = pos + 2 + (count + 1) * size
    return [(first + offsets[i], first + offsets[i + 1]) for i in range(count)], size, first + offsets[-1]


def otf_glyph(data, cmap, code):
    """The glyph id of a code point in the format 4 (platform 3 encoding 1) subtable of the cmap at data[cmap:]."""
    for k in range(struct.unpack_from('>H', data, cmap + 2)[0]):
        platform, encoding, offset = struct.unpack_from('>HHI', data, cmap + 4 + 8 * k)
        p = cmap + offset
        if (platform, encoding) != (3, 1) or struct.unpack_from('>H', data, p)[0] != 4:
            continue
        seg = struct.unpack_from('>H', data, p + 6)[0] // 2
        for s in range(seg):
            end = struct.unpack_from('>H', data, p + 14 + 2 * s)[0]
            start = struct.unpack_from('>H', data, p + 16 + 2 * seg + 2 * s)[0]
            if not start <= code <= end:
                continue
            delta = struct.unpack_from('>h', data, p + 16 + 4 * seg + 2 * s)[0]
            at = p + 16 + 6 * seg + 2 * s
            ro = struct.unpack_from('>H', data, at)[0]
            glyph = code if not ro else struct.unpack_from('>H', data, at + ro + 2 * (code - start))[0]
            return (glyph + delta) & 0xffff if glyph else 0
    raise AssertionError(f'U+{code:04X} is not in the font')


def otf_checksum(data):
    data = bytes(data) + bytes(-len(data) % 4)
    return sum(struct.unpack(f'>{len(data) // 4}I', data)) & 0xffffffff


def gob_offset(x, y, width_bytes, block_height):
    """Byte offset of (x in bytes, y) in a Tegra block-linear surface: 64-byte x 8-row GOBs of 512 bytes, stacked
    block_height GOBs high per block, blocks in rows of the surface's width in GOBs."""
    gobs_wide = (width_bytes + 63) // 64
    block = (y // (8 * block_height)) * gobs_wide + x // 64
    return (block * 512 * block_height + ((y % (8 * block_height)) // 8) * 512 + ((x % 64) // 32) * 256
            + ((y % 8) // 2) * 64 + ((x % 32) // 16) * 32 + (y % 2) * 16 + (x % 16))


def bntx_r8_textures(data, base, names):
    """name -> (width, height, block height, absolute image start, image size) of the named one-mip R8_UNORM
    textures in the BNTX at data[base:] (names without the ^ suffix)."""
    count, table = struct.unpack_from('<IQ', data, base + 0x24)
    found = {}
    for k in range(count):
        info = base + struct.unpack_from('<Q', data, base + table + 8 * k)[0]
        name = base + struct.unpack_from('<Q', data, info + 0x60)[0]
        name = data[name + 2:name + 2 + struct.unpack_from('<H', data, name)[0]].decode().split('^')[0]
        if name not in names:
            continue
        mips, fmt = struct.unpack_from('<H', data, info + 0x16)[0], struct.unpack_from('<I', data, info + 0x1c)[0]
        width, height = struct.unpack_from('<II', data, info + 0x24)
        tile = struct.unpack_from('<H', data, info + 0x12)[0]
        assert fmt == 0x0201 and mips == 1 and tile == 0, f'{name}: format {fmt:#x}, {mips} mips, tile mode {tile}'
        block_height = 1 << struct.unpack_from('<I', data, info + 0x34)[0]
        start = base + struct.unpack_from('<Q', data, base + struct.unpack_from('<Q', data, info + 0x70)[0])[0]
        found[name] = (width, height, block_height, start, struct.unpack_from('<I', data, info + 0x50)[0])
    assert sorted(found) == sorted(names), f'missing textures: {sorted(set(names) - set(found))}'
    return found


def bntx_symbols(data, base, symbols):
    """Draw each texture of symbols (texture name -> PlayStation symbol) as the game's disc with that symbol cut out
    (tools/ps_prompts.py). The disc's centre and radius come from the first texture's own image, so the symbols sit
    where the letters' disc was."""
    textures = bntx_r8_textures(data, base, list(symbols))
    width, height, bh, start, _ = next(iter(textures.values()))
    alpha = [[data[start + gob_offset(x, y, width, bh)] for x in range(width)] for y in range(height)]
    cols = [x for x in range(width) if any(alpha[y][x] > 127 for y in range(height))]
    rows = [y for y in range(height) if any(alpha[y][x] > 127 for x in range(width))]
    cx, cy = (cols[0] + cols[-1] + 1) / 2, (rows[0] + rows[-1] + 1) / 2
    radius = (cols[-1] - cols[0] + 1) / 2
    assert abs(radius - (rows[-1] - rows[0] + 1) / 2) <= 1, 'the letter texture is not a round disc'
    for name, symbol in symbols.items():
        w, h, bh, start, size = textures[name]
        img = ps_prompts.mask(symbol, w, h, cx, cy, radius)
        for y in range(h):
            for x in range(w):
                at = gob_offset(x, y, w, bh)
                assert at < size
                data[start + at] = img[y * w + x]
    return dict(centre=(cx, cy), radius=radius)


def cff_dict(data, pos, end):
    """[(operator, [(value, byte offset, byte length)])] of the CFF DICT at data[pos:end]; real numbers have value
    None."""
    out, operands = [], []
    while pos < end:
        b = data[pos]
        if b <= 21:
            op = (12, data[pos + 1]) if b == 12 else b
            pos += 2 if b == 12 else 1
            out.append((op, operands))
            operands = []
            continue
        if b == 28:
            v, n = struct.unpack_from('>h', data, pos + 1)[0], 3
        elif b == 29:
            v, n = struct.unpack_from('>i', data, pos + 1)[0], 5
        elif b == 30:
            n = 1
            while data[pos + n] & 0x0f != 0x0f and data[pos + n] & 0xf0 != 0xf0:
                n += 1
            v, n = None, n + 1
        elif b <= 246:
            v, n = b - 139, 1
        elif b <= 250:
            v, n = (b - 247) * 256 + data[pos + 1] + 108, 2
        else:
            v, n = -(b - 251) * 256 - data[pos + 1] - 108, 2
        operands.append((v, pos, n))
        pos += n
    return out


def cff_int(v, length):
    """v as a CFF DICT integer of exactly length bytes (the length the old operand had, so nothing moves)."""
    if length == 1 and -107 <= v <= 107:
        return bytes([v + 139])
    if length == 2 and 108 <= v <= 1131:
        return bytes([(v - 108 >> 8) + 247, v - 108 & 0xff])
    if length == 2 and -1131 <= v <= -108:
        return bytes([(-v - 108 >> 8) + 251, -v - 108 & 0xff])
    if length == 3 and -32768 <= v <= 32767:
        return b'\x1c' + struct.pack('>h', v)
    if length == 5:
        return b'\x1d' + struct.pack('>i', v)
    raise AssertionError(f'{v} does not fit a {length}-byte CFF operand')


def cff_index_bytes(items):
    total = sum(len(i) for i in items) + 1
    size = 1 if total < 1 << 8 else 2 if total < 1 << 16 else 3 if total < 1 << 24 else 4
    out, at = bytearray(struct.pack('>HB', len(items), size)), 1
    for i in items + [b'']:
        out += at.to_bytes(size, 'big')
        at += len(i)
    return bytes(out + b''.join(items))


def otf_rebuild(font, tables, new):
    """The OpenType font with the tables in new (tag -> bytes) replaced: every table laid out again in its old order
    at 4-byte alignment, the directory's offsets, lengths and checksums and head's checksum adjustment updated."""
    order = sorted(tables, key=lambda t: tables[t][1])
    out = bytearray(font[:12 + 16 * len(tables)])
    for tag in order:
        entry, offset, length = tables[tag]
        body = new.get(tag, bytes(font[offset:offset + length]))
        out += bytes(-len(out) % 4)
        struct.pack_into('>III', out, entry + 4, otf_checksum(body), len(out), len(body))
        out += body
    out += bytes(-len(out) % 4)
    head = struct.unpack_from('>I', out, tables[b'head'][0] + 8)[0]
    struct.pack_into('>I', out, head + 8, 0)
    struct.pack_into('>I', out, tables[b'head'][0] + 4, otf_checksum(out[head:head + tables[b'head'][2]]))
    struct.pack_into('>I', out, head + 8, (0xb1b0afba - otf_checksum(out)) & 0xffffffff)
    return out


# The button glyphs' disc in the game's button font (scft/External.bfotf), font units: advance 640, left side bearing
# 38 (the disc's left edge), centre (320, 301), radius 282. bfotf_symbols checks the advance and bearing first.
GLYPH_ADVANCE, GLYPH_LSB, GLYPH_CENTRE, GLYPH_RADIUS = 640, 38, (320, 301), 282


def bfotf_symbols(data, symbols):
    """The BFOTF data with the glyph of each code point in symbols (code point -> PlayStation symbol) drawn as the
    disc with that symbol cut out (tools/ps_prompts.py). The CharStrings INDEX is written again with the new items;
    the Private DICT and its subroutines after it move by the difference, and the Top DICT's Private offset follows
    (re-encoded in its old length). The font's tables are then laid out again and the header's size field updated.
    See bfotf_swap for the encryption."""
    key = bytes(x ^ y for x, y in zip(data[4:8], struct.pack('>I', len(data) - 8)))
    plain = bytearray(data[:4]) + bytearray(x ^ key[i % 4] for i, x in enumerate(data[4:]))
    font = plain[8:]
    assert font[:4] == b'OTTO', 'not a CFF OpenType font'
    tables = {}
    for k in range(struct.unpack_from('>H', font, 4)[0]):
        tag, _, offset, length = struct.unpack_from('>4sIII', font, 12 + 16 * k)
        tables[tag] = (12 + 16 * k, offset, length)
    _, cff_at, cff_len = tables[b'CFF ']
    cff = bytearray(font[cff_at:cff_at + cff_len])
    _, _, pos = cff_index(cff, cff[2])                    # Name INDEX after the header
    top, _, _ = cff_index(cff, pos)
    ops = dict(cff_dict(cff, *top[0]))
    assert (12, 30) not in ops, 'CID-keyed CFF'
    cs_at = ops[17][-1][0]
    (priv_size, _, _), (priv_at, priv_pos, priv_len) = ops[18]
    for op in (15, 16):                                   # charset and encoding: before the CharStrings, or predefined
        assert op not in ops or ops[op][-1][0] <= 2 or ops[op][-1][0] < cs_at, f'Top DICT operator {op} after CharStrings'
    strings, _, cs_end = cff_index(cff, cs_at)
    assert cs_end <= priv_at, 'the Private DICT comes before the CharStrings'
    private = dict(cff_dict(cff, priv_at, priv_at + priv_size))
    default_width = private.get(20, [(0, 0, 0)])[-1][0]
    nominal_width = private.get(21, [(0, 0, 0)])[-1][0]
    hmtx, metrics = tables[b'hmtx'][1], struct.unpack_from('>H', font, tables[b'hhea'][1] + 34)[0]
    items = [bytes(cff[a:b]) for a, b in strings]
    for code, symbol in symbols.items():
        glyph = otf_glyph(font, tables[b'cmap'][1], code)
        assert 0 < glyph < min(len(items), metrics), (code, glyph)
        advance, lsb = struct.unpack_from('>Hh', font, hmtx + 4 * glyph)
        assert (advance, lsb) == (GLYPH_ADVANCE, GLYPH_LSB), f'U+{code:04X}: advance {advance}, bearing {lsb}'
        width = None if advance == default_width else advance - nominal_width
        items[glyph] = ps_prompts.charstring(symbol, *GLYPH_CENTRE, GLYPH_RADIUS, width)
    index = cff_index_bytes(items)
    cff[priv_pos:priv_pos + priv_len] = cff_int(priv_at + len(index) - (cs_end - cs_at), priv_len)
    cff = cff[:cs_at] + index + cff[cs_end:]
    font = otf_rebuild(font, tables, {b'CFF ': bytes(cff)})
    plain = bytearray(plain[:4]) + bytearray(struct.pack('>I', len(font))) + font
    return bytes(plain[:4]) + bytes(x ^ key[i % 4] for i, x in enumerate(plain[4:]))


def sarc_replace(data, new):
    """The SARC data with the members named in new (name -> bytes) replaced. The header, SFAT and SFNT stay; the
    members are laid out again in their old order, each at an offset with at least the alignment its old one had."""
    start = struct.unpack_from('<I', data, 12)[0]
    out = bytearray(data[:start])
    for name, node, lo, hi in sorted(sarc_nodes(data), key=lambda n: n[2]):
        align = min(lo & -lo, 0x2000) if lo else 0x2000
        out += bytes(-len(out) % align)
        body = new.get(name, data[lo:hi])
        struct.pack_into('<II', out, node + 8, len(out) - start, len(out) - start + len(body))
        out += body
    struct.pack_into('<I', out, 8, len(out))
    return bytes(out)


COMMON = {'v110': '110', 'v121': '110', 'v143': '140'}   # the version in the name of each game's Common archive
ARCHIVE = f'UI/LayoutArchive/Common.Product.{COMMON[args.version]}.Nin_NX_NVN.blarc.zs'
MEMBERS = {
    'anim/ShortCutBG_00_In.bflan': ('C_CaptureBG_00', 'C_CaptureClearBG_00', 'P_CaptureClearBG_00'),
    'anim/AutoBuilder_00_In.bflan': ('C_CaptureBG_00',),
}
KEY_TEXTURES = (('Nt_KeyTexA_00', 'Nt_KeyTexB_00'), ('Nt_KeyTexX_00', 'Nt_KeyTexY_00'))
KEY_GLYPHS = ((0xE040, 0xE041), (0xE042, 0xE043))   # A B, X Y in scft/External.bfotf
KEY_LETTERS = {'A': 'B', 'B': 'A', 'X': 'Y', 'Y': 'X'}   # the face-button diamond around the action prompt
# prompts = playstation: cross is Switch A, circle B, square X, triangle Y (the xbox layout's buttons)
PS_TEXTURES = {'Nt_KeyTexA_00': 'cross', 'Nt_KeyTexB_00': 'circle', 'Nt_KeyTexX_00': 'square',
               'Nt_KeyTexY_00': 'triangle'}
PS_GLYPHS = {0xE040: 'cross', 0xE041: 'circle', 0xE042: 'square', 0xE043: 'triangle'}
PS_LETTERS = {'A': '×', 'B': '○', 'X': '□', 'Y': '△'}   # times, circle, square, triangle
FONTS = sorted(p for p in files if re.fullmatch(r'Font/Font(_\w+)?\.Nin_NX_NVN\.bfarc\.zs', p))
# The message archive of each language: the newest version in the RomFS (Mals/USen.Product.121.sarc.zs on 1.2.1).
MALS = {}
for p in files:
    m = re.fullmatch(r'Mals/(\w+)\.Product\.(\d+)\.sarc\.zs', p)
    if m and int(m[2]) > MALS.get(m[1], (0, ''))[0]:
        MALS[m[1]] = (int(m[2]), p)
MALS = sorted(p for _, p in MALS.values())
LAYOUT_MSG = 'LayoutMsg/DoCommand_00.msbt'
# The resource size table (System/Resource/ResourceSizeTable.Product.<n>...rsizetable.zs, matched as loosely as
# MacNativeGames make_button_icons.py does), likewise the newest version in the RomFS.
RESTBL = max((p for p in files if p.startswith('System/Resource/ResourceSizeTable.') and p.endswith('.rsizetable.zs')),
             key=lambda p: int((re.search(r'\.(\d+)\.', p) or [0, 0])[1]), default=None)
by_position = args.prompts == 'xbox'   # the face buttons' prompts swapped to the labels (pad_layout = nintendo)
symbols = args.prompts == 'playstation'   # the face buttons' letters drawn as PlayStation symbols
if args.prompts:
    assert FONTS, 'no Font/Font*.Nin_NX_NVN.bfarc.zs in the RomFS'
    assert MALS or not (by_position or symbols), 'no Mals/<language>.Product.<version>.sarc.zs in the RomFS'
    assert RESTBL, 'no System/Resource/ResourceSizeTable.Product.<n>...rsizetable.zs in the RomFS'
    spec = importlib.util.spec_from_file_location('button_icons', Path(__file__).with_name('button_icons.py'))
    icons = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(icons)
    assert icons.TEXT_ARCHIVE in FONTS, f'no {icons.TEXT_ARCHIVE} in the RomFS'
if symbols:   # loaded by path: the release's embedded Python does not put the script's folder on sys.path
    spec = importlib.util.spec_from_file_location('ps_prompts', Path(__file__).with_name('ps_prompts.py'))
    ps_prompts = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ps_prompts)

with (args.romfs / 'romfs.bin').open('rb') as image:
    def read(path):
        f = files[path]
        image.seek(f['offset'])
        b = image.read(f['size'])
        assert len(b) == f['size']
        return b
    dictionaries = {}
    pack = zstd.decompress(read('Pack/ZsDic.pack.zs'))
    for name, (lo, hi) in sarc_members(pack).items():
        d = zstd.ZstdDict(pack[lo:hi])
        dictionaries[d.dict_id] = d
    prompt_files = (FONTS + (MALS if by_position or symbols else []) + [RESTBL]) if args.prompts else []
    originals = {path: read(path) for path in [ARCHIVE] + prompt_files}
raw = originals[ARCHIVE]
info = zstd.get_frame_info(raw)
dictionary = dictionaries[info.dictionary_id]
original = zstd.decompress(raw, zstd_dict=dictionary)
blarc = bytearray(original)
assert len(blarc) == info.decompressed_size
members = sarc_members(blarc)
patched = {}
for member, panes in MEMBERS.items():
    lo, hi = members[member]
    changed = []
    for name, ko, keys in flvi_keys(blarc, lo):
        if name not in panes:
            continue
        assert [(f, v) for f, v in keys] == [(0.0, 0), (1.0, 1)], (member, name, keys)
        struct.pack_into('<f', blarc, ko + 8, 0.0)
        changed.append(name)
    assert sorted(changed) == sorted(panes), (member, changed)
    patched[member] = changed
layout = []
if args.aspect:
    width, height = (int(v) for v in args.aspect.lower().split('x'))
    spec = importlib.util.spec_from_file_location('ultrawide_layout', Path(__file__).with_name('ultrawide_layout.py'))
    ultrawide = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ultrawide)
    layout = ultrawide.apply(blarc, members, width, height)
diff = [i for i in range(len(original)) if original[i] != blarc[i]]
touched = [members[m] for m in (*MEMBERS, *layout)]
assert diff and all(any(lo <= i < hi for lo, hi in touched) for i in diff), diff
outputs = {ARCHIVE: bytes(blarc)}   # RomFS path -> new contents (before compression)
prompts = None
if args.prompts:
    sizes = {ARCHIVE: len(original)}   # RomFS path -> size of the original's contents

    def unpack(path):
        frame = zstd.get_frame_info(originals[path])
        data = zstd.decompress(originals[path], zstd_dict=dictionaries.get(frame.dictionary_id))
        sizes[path] = len(data)
        return data
    if symbols:
        letters, drawn, hud = {}, {}, None
        disc = bntx_symbols(blarc, members['timg/__Combined.bntx'][0], PS_TEXTURES)
        for member, (lo, hi) in members.items():
            if member.endswith('.bflyt'):
                changed = bflyt_letters(blarc, lo, hi, PS_LETTERS)
                if changed:
                    letters[member] = changed
        assert 'blyt/DoCommand_00.bflyt' in letters, 'no letter panes in blyt/DoCommand_00.bflyt'
        outputs[ARCHIVE] = bytes(blarc)
        for path in FONTS:
            bfarc = unpack(path)
            lo, hi = sarc_members(bfarc)[icons.FONT]
            outputs[path] = sarc_replace(bfarc, {icons.FONT: bfotf_symbols(bfarc[lo:hi], PS_GLYPHS)})
    else:
        archives = {path: unpack(path) for path in FONTS}
        text = sarc_members(archives[icons.TEXT_ARCHIVE])
        missing = [name for name in icons.TEXT_FONTS if name not in text]
        assert not missing, f'no {", ".join(missing)} in {icons.TEXT_ARCHIVE}'
        drawn = icons.xbox_icons(icons.text_glyphs(archives[icons.TEXT_ARCHIVE][slice(*text[name])]
                                                   for name in icons.TEXT_FONTS))
        bntx = members['timg/__Combined.bntx'][0]
        letters = {}
        if by_position:
            bntx_swap(blarc, bntx, KEY_TEXTURES)
            for member, (lo, hi) in members.items():
                if member.endswith('.bflyt'):
                    changed = bflyt_letters(blarc, lo, hi, KEY_LETTERS)
                    if changed:
                        letters[member] = changed
            assert 'blyt/DoCommand_00.bflyt' in letters, 'no letter panes in blyt/DoCommand_00.bflyt'
        hud = icons.bntx_redraw(blarc, bntx, icons.HUD_TEXTURES, drawn)
        outputs[ARCHIVE] = bytes(blarc)
        fonts = {}   # each distinct font is drawn once (1.4.3's four archives hold the same one)
        for path, bfarc in archives.items():
            lo, hi = sarc_members(bfarc)[icons.FONT]
            otf, key = icons.bfotf_decode(bfarc[lo:hi])
            if otf not in fonts:
                fonts[otf] = icons.redraw_font(otf, drawn, KEY_GLYPHS if by_position else ())
            outputs[path] = sarc_replace(bfarc, {icons.FONT: icons.bfotf_encode(fonts[otf], key, bfarc[lo:hi])})
    messages = {}
    for path in MALS if by_position or symbols else []:
        mals = bytearray(unpack(path))
        lo, hi = sarc_members(mals)[LAYOUT_MSG]
        msbt = bytearray(mals[lo:hi])
        changed = msbt_letters(msbt, PS_LETTERS if symbols else KEY_LETTERS)
        assert len(changed) == sum(len(v) for v in letters.values()), (path, changed)
        mals[lo:hi] = msbt
        outputs[path] = bytes(mals)
        messages[path] = len(changed)
    # An archive that grew needs as much more room in the resource size table, or the game stops when it loads it.
    grown = {path: len(data) - sizes[path] for path, data in outputs.items() if len(data) > sizes[path]}
    if grown:
        outputs[RESTBL] = icons.grow_restbl(unpack(RESTBL), grown)
    prompts = dict(style=args.prompts, textures=KEY_TEXTURES if by_position else [], letters=letters,
                   messages=messages, hud=hud, icons=len(drawn), fonts=FONTS, grown=grown)
    if symbols:
        prompts['disc'] = disc

lines = []
for path, contents in outputs.items():
    raw = originals[path]
    dictionary = dictionaries.get(zstd.get_frame_info(raw).dictionary_id)
    data = zstd.compress(contents, level=19, zstd_dict=dictionary)
    assert zstd.decompress(data, zstd_dict=dictionary) == contents
    assert zstd.get_frame_info(data).dictionary_id == zstd.get_frame_info(raw).dictionary_id
    target = args.out / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    lines.append('\t'.join([path, str(len(data)), hashlib.sha256(data).hexdigest(), hashlib.sha256(raw).hexdigest()]))
(args.out / 'manifest.tsv').write_text('# path\tsize\tsha256\toriginal_sha256 (tools/make_romfs_patches.py)\n'
                                       + ''.join(line + '\n' for line in lines))
print(json.dumps(dict(status='pass', path=ARCHIVE, members=patched, changed_bytes=len(diff),
                      ultrawide_layout=dict(aspect=args.aspect, members=len(layout)) if args.aspect else None,
                      prompts=prompts)))
