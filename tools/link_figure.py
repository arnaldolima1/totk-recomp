"""Builds the figure the game draws for each friend in a multiplayer room (runtime/link_figure.c): Link in the Hylian
outfit, posed with the arms down, as one textured triangle mesh. Made from the player's own game files; the result
stays in the profile (never in this repository).

    python tools/link_figure.py <models dir> <romfs dir> <out.bin> [preview.png]
    python tools/link_figure.py --mcsharp <mcsharp-cli> --work <dir> <romfs dir> <out.bin>

<models dir> holds the decompressed BFRES of the parts (<name>.bfres or <name>.bfres/<name>.bfres), decompressed from
the RomFS's Model/<name>.bfres.mc by a MeshCodec decoder, and Player_Animation.anim.bfres; the textures come from the
RomFS's TexToGo/<name>.txtg. With --mcsharp the tool makes that folder itself in <dir> (prepare): the parts through
McSharp's command line (BladesawStudios/McSharp, MIT; the release ships it in bin/mcsharp), the animations from
Model/Player_Animation.anim.bfres.zs with the RomFS's zstd dictionaries (Pack/ZsDic.pack.zs). The installer runs this
form (installer/steps.py, prepare) and keeps the result as profile/link-figure.bin.

Output (little-endian): b'LNKF', u32 version 4, u32 vertex count, u32 triangle count, u32 atlas side, u32 ASTC
picture count; per vertex 3 floats (metres, model space: x right, y up, z forward); per triangle 3 u32 indices; per
triangle one u8 material class (CLASSES; bit 7: no texture, the runtime colours it by class); per triangle 6 floats,
its corners' atlas coordinates (0..1); the atlas, side x side RGBA8 rows; then per ASTC picture u32 x, y, width,
height, block width, block height, byte count and its linear blocks, which the runtime decodes into the atlas there;
then u32 bone count, u32 animation count, per vertex four (u16 bone, u16 weight of 65535); the skeleton: u32 root
bone (the one carrying the animation's own motion, which the runtime leaves out, 0xffffffff none), per bone i32 parent (-1 none), its bind local
transform as 9 floats (scale xyz, euler XYZ rotation, translation xyz) and its inverse bind world matrix (row-major 3x4,
12 floats), then per bone its name (32 bytes); then per animation (ANIMS first, the others in the file's order) its name (48 bytes), u32 frame count, u32
flags (bit 0 loops), f32 ground speed (m/s, ANIMS only, else 0), u32 animated bone count, u32 curve count, u32 key
count; per animated bone u16 bone, u16 curve count and 9 floats, its local transform where no curve moves it; per curve
u8 channel (0..8, the 9 floats' order), u8 0, u16 key count; per key u16 frame x 32 and 4 halves, the cubic's
coefficients over the key's segment (t 0..1) with the curve's scale and offset already in. The runtime evaluates them
(runtime/link_figure.c link_figure_pose). Frames run at 30 a second.
"""
import math
import struct
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import bfres  # noqa: E402
import txtg  # noqa: E402

# (file, shapes to keep by name prefix; None keeps all but the excluded ones)
PARTS = [
    ('Link.Link_RaulSkin', ('Face__', 'Eye_', 'Eyelashes', 'Teeth', 'Earring')),
    ('Link.Link_RaulSkin_RightHand', None),
    ('Armor_Default.Armor_Default_Head', None),
    ('Armor_001.Armor_001_RaulSkin_Upper', None),
    ('Armor_001.Armor_001_Lower', None),
]
EXCLUDE = ('Ring_',)   # the sage rings show one by one in the game

# Material classes, in the order runtime/link_figure.c colours them.
CLASSES = ['skin', 'hair', 'tunic', 'trousers', 'leather', 'eye', 'dark', 'metal', 'rauru']
RULES = [('Mt_Hair', 'hair'), ('Mt_Eyeball', 'eye'), ('Mt_Eyelashes', 'dark'), ('Mt_Face', 'skin'), ('Mt_Head', 'skin'),
         ('Skin', 'skin'), ('Mt_Upper_001', 'tunic'), ('Mt_Lower_001', 'trousers'), ('Belt', 'leather'),
         ('Kusari', 'metal'), ('Earring', 'metal'), ('Monyou', 'rauru'), ('Ornaments', 'rauru'), ('Ring', 'metal')]

def material_class(name):
    for key, cls in RULES:
        if key in name:
            return CLASSES.index(cls)
    return CLASSES.index('leather')


def euler_xyz(rx, ry, rz):
    cx, sx, cy, sy, cz, sz = math.cos(rx), math.sin(rx), math.cos(ry), math.sin(ry), math.cos(rz), math.sin(rz)
    return [[cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz],
            [cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz],
            [-sy, sx * cy, cx * cy]]


def mul33(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def local(b):
    return trs(b['scale'], b['rot'][:3], b['pos'])


def mul34(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) + (a[i][3] if j == 3 else 0) for j in range(4)] for i in range(3)]


def worlds(sk, pose=None):
    out = []
    for b in sk['bones']:
        m = local(b)
        out.append(mul34(out[b['parent']], m) if b['parent'] >= 0 else m)
    return out


def apply(m, p):
    return [m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + m[i][3] for i in range(3)]


def load(models, name):
    for p in (models / f'{name}.bfres' / f'{name}.bfres', models / f'{name}.bfres'):
        if p.is_file():
            return bfres.read_bfres(p.read_bytes())
    raise FileNotFoundError(f'{name}.bfres not in {models}')


class Skeleton:
    """The bones of every part joined by name (they share Link's rig): parent, bind local transform (euler XYZ), and the
    bind world matrices."""
    def __init__(self):
        self.names, self.parent, self.bind = [], [], []
        self.index = {}

    def add(self, sk):
        for b in sk['bones']:
            if b['name'] in self.index:
                continue
            parent = sk['bones'][b['parent']]['name'] if b['parent'] >= 0 else None
            self.index[b['name']] = len(self.names)
            self.names.append(b['name'])
            self.parent.append(self.index[parent] if parent is not None else -1)
            self.bind.append((list(b['scale']), list(b['rot'][:3]), list(b['pos'])))

    def worlds(self, locals_):
        out = []
        for i, m in enumerate(locals_):
            out.append(mul34(out[self.parent[i]], m) if self.parent[i] >= 0 else m)
        return out

    def bind_worlds(self):
        return self.worlds([trs(*b) for b in self.bind])


def trs(scale, rot, pos):
    r = euler_xyz(*rot)
    return [[r[i][0] * scale[0], r[i][1] * scale[1], r[i][2] * scale[2], pos[i]] for i in range(3)]


def build(models):
    """Vertices in the bind pose (model space), up to four (bone, weight) per vertex over the joined skeleton, and the
    triangles with their material class, texture and corners' texture coordinates."""
    skel = Skeleton()
    parts = []
    for name, keep in PARTS:
        for m in load(models, name)['models']:
            if m['skeleton']['rotation'] != 'euler_xyz':
                raise ValueError(f'{name}: skeleton rotation {m["skeleton"]["rotation"]} not handled')
            skel.add(m['skeleton'])
            parts.append((name, keep, m))
    verts, weights, tris, classes, uvs, texs = [], [], [], [], [], []
    for name, keep, m in parts:
        sk = m['skeleton']
        own = worlds(sk, None)
        m2b = sk['matrix_to_bone']
        joined = [skel.index[b['name']] for b in sk['bones']]
        for s in m['shapes']:
            if any(s['name'].startswith(x) for x in EXCLUDE):
                continue
            if keep and not any(s['name'].startswith(x) for x in keep):
                continue
            vb = s['vertex_buffer']
            P, I, Wt = vb['attrs']['_p0'], vb['attrs'].get('_i0'), vb['attrs'].get('_w0')
            base = len(verts)
            for k, p in enumerate(P):
                p = p[:3]
                if s['skin'] == 0:
                    verts.append(apply(own[s['bone']], p))
                    weights.append([(joined[s['bone']], 1.0)])
                elif s['skin'] == 1:
                    bone = m2b[int(I[k][0])]
                    verts.append(apply(own[bone], p))
                    weights.append([(joined[bone], 1.0)])
                else:   # smooth skinning: the position is already in model space
                    verts.append(p)
                    ws = []
                    for j in range(s['skin']):
                        w = Wt[k][j] if Wt else (1.0 if j == 0 else 0.0)
                        if w > 0:
                            ws.append((joined[m2b[int(I[k][j])]], w))
                    total = sum(w for _, w in ws) or 1.0
                    weights.append([(b, w / total) for b, w in ws][:4])
            mat = m['materials'][s['material']]
            cls = material_class(mat['name'])
            tex = mat['textures'][0] if mat['textures'] else None
            U = vb['attrs'].get('_u0')
            idx = s['meshes'][0]['indices']
            for q in range(0, len(idx) - 2, 3):
                tris.append((base + idx[q], base + idx[q + 1], base + idx[q + 2]))
                classes.append(cls)
                texs.append(tex if U else None)
                uvs.append([tuple(U[idx[q + c]][:2]) for c in range(3)] if U else [(0, 0)] * 3)
    return skel, verts, weights, tris, classes, uvs, texs


# The animations the figure plays by the friend's speed when their game names none (an older game): standing,
# walking, running, dashing. They lead the list, in this order (runtime/link_figure.h LF_*); every other skeletal
# animation of Player_Animation follows, for the one a friend's game names (runtime/net_mp.c POS lines).
ANIMS = ['Nml_Wait', 'Nml_Move_Walk', 'Nml_Move_Run', 'Nml_Move_Dash']


def channel(target):
    """A curve's place among a bone's 9 floats (scale xyz, rotation xyz, translation xyz); None for a rotation's w."""
    if 0x04 <= target <= 0x0C:
        return (target - 0x04) // 4
    if 0x20 <= target <= 0x28:
        return 3 + (target - 0x20) // 4
    if 0x10 <= target <= 0x18:
        return 6 + (target - 0x10) // 4
    return None


def pack_anim(skel, anim):
    """One animation for the file: its bones (joined index, curves, the local transform where no curve moves it) and
    the speed its root travels forward (m/s at 30 frames a second, from its translation z; the runtime leaves the
    root's motion out, the friend's own place and facing move the figure)."""
    bones, curves, keys = bytearray(), bytearray(), bytearray()
    nb = nc = nk = 0
    root_z = None
    for name, ba in anim['bones'].items():
        i = skel.index.get(name)
        if i is None:
            continue
        scale, rot, pos = (list(v) for v in skel.bind[i])
        if 'scale' in ba['base']:
            scale = list(ba['base']['scale'])
        if 'rot' in ba['base']:
            rot = list(ba['base']['rot'][:3])
        if 'pos' in ba['base']:
            pos = list(ba['base']['pos'])
        own = []
        for c in ba['curves']:
            ch = channel(c['target'])
            if ch is None or not c['frames']:
                continue
            own.append((ch, c))
            if name == 'Root' and ch == 8:
                root_z = c
        bones += struct.pack('<HH9f', i, len(own), *scale, *rot, *pos)
        nb += 1
        for ch, c in own:
            curves += struct.pack('<BBH', ch, 0, len(c['frames']))
            sc, off = c['scale'], c['offset']
            for f, k in zip(c['frames'], c['keys']):
                k = (list(k) + [0.0] * 4)[:4]
                if c['type'] == 1:      # linear: value and delta
                    k = [k[0], k[1], 0.0, 0.0]
                elif c['type'] == 2:    # one value a frame
                    k = [k[0], 0.0, 0.0, 0.0]
                co = [k[0] * sc + off, k[1] * sc, k[2] * sc, k[3] * sc]
                keys += struct.pack('<H4e', min(65535, max(0, round(f * 32))), *(max(-65504.0, min(65504.0, v)) for v in co))
            nc += 1
            nk += len(c['frames'])
    frames = max(1, anim['frames'])
    speed = 0.0
    if root_z is not None and frames > 1:
        travel = bfres.curve_at(root_z, frames - 1) - bfres.curve_at(root_z, 0)
        speed = abs(travel) / ((frames - 1) / 30.0)
    return frames, speed, (nb, nc, nk, bytes(bones + curves + keys))


def inverse34(m):
    """The inverse of a 3x4 affine transform."""
    a = [row[:3] for row in m]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
           a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    inv = [[(a[(j + 1) % 3][(i + 1) % 3] * a[(j + 2) % 3][(i + 2) % 3] -
             a[(j + 1) % 3][(i + 2) % 3] * a[(j + 2) % 3][(i + 1) % 3]) / det for j in range(3)] for i in range(3)]
    t = [m[0][3], m[1][3], m[2][3]]
    return [[inv[i][0], inv[i][1], inv[i][2], -(inv[i][0] * t[0] + inv[i][1] * t[1] + inv[i][2] * t[2])] for i in range(3)]


ATLAS = 1024
PAD = 2


class RomFS:
    def __init__(self, root):
        self.root = Path(root)
        self.idx = {f['path']: f for f in json.loads((self.root / 'index.json').read_text(encoding='utf-8'))['files']}
        self.rom = (self.root / 'romfs.bin').open('rb')

    def read(self, path):
        f = self.idx.get(path)
        if not f:
            return None
        self.rom.seek(f['offset'])
        return self.rom.read(f['size'])


def build_atlas(romfs, names):
    """{name: (x, y, w, h)} in an ATLAS x ATLAS RGBA8 picture, the picture, and the ASTC pictures to decode there. A
    white 8x8 square at (0, 0) serves the untextured triangles. Shelf packing, tallest first; a texture that does not
    fit is left out (its triangles go untextured)."""
    pics = {}
    for n in names:
        data = romfs.read(f'TexToGo/{n}.txtg') or romfs.read(f'TexToGo/{n}.0.txtg')
        if data is None:
            print(f'  texture {n}: not in the RomFS')
            continue
        try:
            w, h, kind, bw, bh, px = txtg.read_txtg(data, 256)
        except ValueError as e:
            print(f'  texture {n}: {e}')
            continue
        if px is None:
            print(f'  texture {n}: format {kind} not handled')
            continue
        pics[n] = (w, h, kind, bw, bh, px)
    atlas = bytearray(ATLAS * ATLAS * 4)
    for y in range(8):
        atlas[y * ATLAS * 4:(y * ATLAS + 8) * 4] = bytes([255]) * 32
    place, astc = {}, []
    x, y, shelf = 8 + PAD, 0, 8
    for n in sorted(pics, key=lambda k: -pics[k][1]):
        w, h, kind, bw, bh, px = pics[n]
        if x + w > ATLAS:
            x, y, shelf = 0, y + shelf + PAD, 0
        if y + h > ATLAS:
            print(f'  texture {n}: no room in the atlas')
            continue
        place[n] = (x, y, w, h)
        if kind == 'astc':
            astc.append((x, y, w, h, bw, bh, px))
        else:
            for r in range(h):
                atlas[((y + r) * ATLAS + x) * 4:((y + r) * ATLAS + x + w) * 4] = px[r * w * 4:(r + 1) * w * 4]
        x += w + PAD
        shelf = max(shelf, h)
    return place, atlas, astc


def atlas_uv(rect, tri_uv):
    """A triangle's corners in the atlas: shifted by whole tiles to start in 0..1, then clamped into the texture with a
    half-texel inset."""
    x, y, w, h = rect
    fu = math.floor(min(u for u, _ in tri_uv))
    fv = math.floor(min(v for _, v in tri_uv))
    out = []
    for u, v in tri_uv:
        u = min(max(u - fu, 0.5 / w), 1 - 0.5 / w)
        v = min(max(v - fv, 0.5 / h), 1 - 0.5 / h)
        out += [(x + u * w) / ATLAS, (y + v * h) / ATLAS]
    return out


def write(path, verts, weights, tris, classes, uvs, texs, place, atlas, astc, skel, anims):
    out = bytearray(b'LNKF')
    out += struct.pack('<IIIII', 4, len(verts), len(tris), ATLAS, len(astc))
    for v in verts:
        out += struct.pack('<3f', *v)
    for t in tris:
        out += struct.pack('<3I', *t)
    flags = bytearray()
    coords = bytearray()
    for i in range(len(tris)):
        rect = place.get(texs[i]) if texs[i] else None
        flags.append(classes[i] | (0 if rect else 0x80))
        coords += struct.pack('<6f', *(atlas_uv(rect, uvs[i]) if rect else [4 / ATLAS] * 6))
    out += flags + coords + atlas
    for x, y, w, h, bw, bh, blocks in astc:
        out += struct.pack('<7I', x, y, w, h, bw, bh, len(blocks)) + blocks
    out += struct.pack('<II', len(skel.names), len(anims))
    for ws in weights:   # four (u16 bone, u16 weight of 65535) per vertex
        ws = (ws + [(0, 0.0)] * 4)[:4]
        q = [round(w * 65535) for _, w in ws]
        q[0] += 65535 - sum(q)
        for (b, _), w in zip(ws, q):
            out += struct.pack('<HH', b, w)
    out += struct.pack('<I', skel.index.get('Root', 0xffffffff))
    for i, w in enumerate(skel.bind_worlds()):
        scale, rot, pos = skel.bind[i]
        m = inverse34(w)
        out += struct.pack('<i9f12f', skel.parent[i], *scale, *rot, *pos, *m[0], *m[1], *m[2])
    for name in skel.names:
        out += name.encode()[:31].ljust(32, b'\0')
    for name, frames, loop, speed, (nb, nc, nk, blob) in anims:
        out += name.encode()[:47].ljust(48, b'\0') + struct.pack('<IIfIII', frames, 1 if loop else 0, speed, nb, nc, nk)
        out += blob
    Path(path).write_bytes(out)


def preview(path, verts, tris, classes):
    from PIL import Image, ImageDraw
    colors = {'skin': (233, 190, 150), 'hair': (226, 186, 90), 'tunic': (70, 150, 80), 'trousers': (200, 180, 140),
              'leather': (120, 80, 50), 'eye': (240, 240, 240), 'dark': (40, 30, 30), 'metal': (150, 170, 190),
              'rauru': (90, 200, 160)}
    H = 900
    img = Image.new('RGB', (H * 2, H), (40, 44, 52))
    d = ImageDraw.Draw(img)
    ys = [v[1] for v in verts]
    lo, sc = min(ys), (H - 60) / (max(ys) - min(ys))
    for view in range(2):
        ax = 2 if view == 0 else 0
        order = sorted(range(len(tris)), key=lambda i: sum(verts[j][ax] for j in tris[i]) * (1 if view == 0 else -1))
        for i in order:
            a, b, c = (verts[j] for j in tris[i])
            u = [b[k] - a[k] for k in range(3)]
            v = [c[k] - a[k] for k in range(3)]
            n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
            ln = math.sqrt(sum(x * x for x in n)) or 1
            shade = 0.35 + 0.65 * abs(n[ax] / ln)
            col = tuple(int(x * shade) for x in colors[CLASSES[classes[i]]])
            d.polygon([(H * view + H / 2 + (p[0] if view == 0 else p[2]) * sc, H - 30 - (p[1] - lo) * sc) for p in (a, b, c)],
                      fill=col)
    img.save(path)


def sarc_members(data):
    """name -> bytes of every member of a SARC archive."""
    assert data[:4] == b'SARC' and data[6:8] == b'\xff\xfe', 'not a little-endian SARC'
    header, start = struct.unpack_from('<H', data, 4)[0], struct.unpack_from('<I', data, 12)[0]
    count = struct.unpack_from('<H', data, header + 6)[0]
    names = header + 12 + count * 16 + 8
    out = {}
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from('<4I', data, header + 12 + i * 16)
        at = names + (attribute & 0xffffff) * 4
        out[data[at:data.index(0, at)].decode()] = data[start + lo:start + hi]
    return out


def prepare(romfs, mcsharp, work):
    """The models folder build and main read, made in work from the player's own RomFS: each part's
    Model/<name>.bfres.mc through McSharp's decoder, and the animations unpacked from their zstd frame."""
    import subprocess
    work.mkdir(parents=True, exist_ok=True)
    for name, _ in PARTS:
        data = romfs.read(f'Model/{name}.bfres.mc')
        if data is None:
            raise FileNotFoundError(f'Model/{name}.bfres.mc is not in the RomFS')
        (work / f'{name}.bfres.mc').write_bytes(data)
    r = subprocess.run([str(mcsharp), str(work), str(work)], capture_output=True, text=True,
                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if r.returncode:
        raise RuntimeError(f'mcsharp exit {r.returncode}: {r.stdout[-800:]} {r.stderr[-800:]}')
    dicts = {}
    for blob in sarc_members(txtg.unzstd(romfs.read('Pack/ZsDic.pack.zs'))).values():
        if blob[:4] == b'\x37\xa4\x30\xec':   # a zstd dictionary: its id follows the magic
            dicts[struct.unpack_from('<I', blob, 4)[0]] = txtg.zstd_dict(blob)
    frame = romfs.read('Model/Player_Animation.anim.bfres.zs')
    if frame is None:
        raise FileNotFoundError('Model/Player_Animation.anim.bfres.zs is not in the RomFS')
    # the frame header names its dictionary: a 1-, 2- or 4-byte id after the descriptor (and window byte)
    fhd = frame[4]
    did_size = (0, 1, 2, 4)[fhd & 3]
    at = 5 + (0 if fhd & 0x20 else 1)
    did = int.from_bytes(frame[at:at + did_size], 'little') if did_size else 0
    (work / 'Player_Animation.anim.bfres').write_bytes(txtg.unzstd(frame, dicts.get(did)))
    return work


def main(argv):
    if len(argv) > 1 and argv[1] == '--mcsharp':   # --mcsharp <exe> --work <dir> <romfs dir> <out.bin>
        if len(argv) < 7 or argv[3] != '--work':
            sys.exit(__doc__)
        romfs = RomFS(argv[5])
        models = prepare(romfs, Path(argv[2]), Path(argv[4]))
        argv = [argv[0], str(models), argv[5], argv[6]]
    skel, verts, weights, tris, classes, uvs, texs = build(Path(argv[1]))
    romfs = RomFS(argv[2])
    names = sorted({x for x in texs if x})
    place, atlas, astc = build_atlas(romfs, names)
    anim_file = Path(argv[1]) / 'Player_Animation.anim.bfres'
    found = bfres.read_skeletal_anims(anim_file.read_bytes(), None)
    anims = []
    for n in ANIMS + [n for n in found if n not in ANIMS]:
        if n in found:
            frames, speed, packed = pack_anim(skel, found[n])
            anims.append((n, frames, found[n]['loop'], speed if n in ANIMS else 0.0, packed))
            if n in ANIMS:
                print(f'  animation {n}: {frames} frames, {speed:.2f} m/s')
    write(argv[3], verts, weights, tris, classes, uvs, texs, place, atlas, astc, skel, anims)
    print(f'{len(verts)} vertices, {len(tris)} triangles, {len(skel.names)} bones, {len(place)} textures '
          f'({len(astc)} ASTC), {len(anims)} animations -> {argv[3]}')
    if len(argv) > 4:
        preview(argv[4], verts, tris, classes)


if __name__ == '__main__':
    main(sys.argv)
