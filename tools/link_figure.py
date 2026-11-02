"""Builds the figure the game draws for each friend in a multiplayer room (runtime/link_figure.c): Link in the Hylian
outfit, posed with the arms down, as one textured triangle mesh. Made from the player's own game files; the result
stays in the profile (never in this repository).

    python tools/link_figure.py <models dir> <romfs dir> <out.bin> [preview.png]
    python tools/link_figure.py --mcsharp <mcsharp-cli> --work <dir> <romfs dir> <out.bin> [--armor <armor.bin>]

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
height, block width (bit 8: lay it over the skin's tone by its alpha), block height, byte count and its linear
blocks, which the runtime decodes into the atlas there;
then u32 bone count, u32 animation count, per vertex four (u16 bone, u16 weight of 65535); the skeleton: u32 root
bone (the one carrying the animation's own motion, which the runtime leaves out, 0xffffffff none), per bone i32 parent (-1 none), its bind local
transform as 9 floats (scale xyz, euler XYZ rotation, translation xyz) and its inverse bind world matrix (row-major 3x4,
12 floats), then per bone its name (32 bytes); then per animation (ANIMS first, the others in the file's order) its name (48 bytes), u32 frame count, u32
flags (bit 0 loops), f32 ground speed (m/s, ANIMS only, else 0), u32 animated bone count, u32 curve count, u32 key
count; per animated bone u16 bone, u16 curve count and 9 floats, its local transform where no curve moves it; per curve
u8 channel (0..8, the 9 floats' order), u8 0, u16 key count; per key u16 frame x 32 and 4 halves, the cubic's
coefficients over the key's segment (t 0..1) with the curve's scale and offset already in. The runtime evaluates them
(runtime/link_figure.c link_figure_pose). Frames run at 30 a second. Last, b'PALM', u32 entry count and per entry u16
bone (0xffff none) and u8 rigid, u8 0: the figure's bone each entry of Link_RaulSkin's skinning palette moves, in its
matrix_to_bone order (the game's palette a friend's game reads, runtime/frame_rate.c game_player_body); a smooth
entry is the bone's pose times its inverse bind, a rigid one the pose itself.

With --armor the tool also writes the armor library the runtime dresses each friend's figure from
(runtime/link_figure.c link_figure_dressed). Strings are a u8 length and its bytes; a group list is a u8 count, then
per group its name, a u8 count and those material names. b'LNKA', u32 version 2, u32 model count, u32 piece count;
the body's material groups (the Player's EquipmentUserParam); per piece (every Armor_*_{Head,Upper,Lower} actor) its
name (48 bytes), i32 its model (-1 none), u8 count and the names of the groups it hides, then its own model's groups
(its ArmorParam); then per model its name (48 bytes; the first is Link_Body, Link's own model), u32 vertex count, u32
triangle count, u32 atlas side, u32 ASTC picture count, u32 the atlas's zstd size, u16 material count and their
names; per vertex 3 floats, then four (u16 bone, u16 weight of 65535) over the figure's skeleton; per triangle 3 u32
indices, then one u8 class each, then one u16 material each, then 6 floats each (its corners in the atlas); the atlas
(side x side RGBA8, zstd); the ASTC pictures as in the figure.
"""
import math
import re
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
# Link's right arm is Rauru's: its skin (Mt_Upper_Skin_RaulSkin_*) lays a picture of its own over the skin by a second
# UV set: Link_RaulSkin_Monyou_Gn5, the arm's dark olive with its darker patches, whose alpha says where it covers the
# skin (the whole forearm, patches above the elbow). The figure draws those triangles with that picture by that UV set;
# it is ASTC, so the runtime lays it over the skin's tone as it decodes it (OVER_SKIN).
ZONAI_ARM = ('Mt_Upper_Skin_RaulSkin', 'Link_RaulSkin_Monyou_Gn5')
OVER_SKIN = {ZONAI_ARM[1]}   # ASTC pictures flagged (block width bit 8) to be laid over the skin's tone where clear
# the sage rings, and the arm's patterns that go with each, show one by one in the game as they are earned
EXCLUDE = ('Ring_', 'Upper_Monyou__Mt_Monyou_Ring_')

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


class Mesh:
    """Vertices in the bind pose (model space), up to four (bone, weight) per vertex over the joined skeleton, and the
    triangles with their material class, texture and corners' texture coordinates."""
    def __init__(self):
        self.verts, self.weights, self.tris, self.classes, self.uvs, self.texs = [], [], [], [], [], []
        self.mats = []   # per triangle its material's name (the game hides armor and body parts by material)

    def add(self, m, keep, joined):
        """The shapes of model m (those keep names by prefix, every one for None), its bones mapped to the joined
        skeleton's by joined."""
        sk = m['skeleton']
        own = worlds(sk, None)
        m2b = sk['matrix_to_bone']
        for s in m['shapes']:
            if any(s['name'].startswith(x) for x in EXCLUDE):
                continue
            if keep and not any(s['name'].startswith(x) for x in keep):
                continue
            vb = s['vertex_buffer']
            P, I, Wt = vb['attrs']['_p0'], vb['attrs'].get('_i0'), vb['attrs'].get('_w0')
            if s['skin'] > 4 and I and '_i1' in vb['attrs']:   # over four bones: the next four in _i1 and _w1
                I = [list(a)[:4] + list(b) for a, b in zip(I, vb['attrs']['_i1'])]
                if Wt and '_w1' in vb['attrs']:
                    Wt = [list(a)[:4] + list(b) for a, b in zip(Wt, vb['attrs']['_w1'])]
            base = len(self.verts)
            for k, p in enumerate(P):
                p = p[:3]
                if s['skin'] == 0:
                    self.verts.append(apply(own[s['bone']], p))
                    self.weights.append([(joined[s['bone']], 1.0)])
                elif s['skin'] == 1:
                    bone = m2b[int(I[k][0])]
                    self.verts.append(apply(own[bone], p))
                    self.weights.append([(joined[bone], 1.0)])
                else:   # smooth skinning: the position is already in model space
                    self.verts.append(p)
                    ws = []
                    for j in range(min(s['skin'], len(I[k]))):
                        w = (Wt[k][j] if j < len(Wt[k]) else 0.0) if Wt else (1.0 if j == 0 else 0.0)
                        if w > 0:
                            ws.append((joined[m2b[int(I[k][j])]], w))
                    total = sum(w for _, w in ws) or 1.0
                    self.weights.append([(b, w / total) for b, w in ws][:4])
            mat = m['materials'][s['material']]
            cls = material_class(mat['name'])
            tex = mat['textures'][0] if mat['textures'] else None
            U = vb['attrs'].get('_u0')
            if mat['name'].startswith(ZONAI_ARM[0]) and ZONAI_ARM[1] in mat['textures'] and '_u1' in vb['attrs']:
                tex, U = ZONAI_ARM[1], vb['attrs']['_u1']   # the Zonai arm's own picture, by its second UV set
            idx = s['meshes'][0]['indices']
            for q in range(0, len(idx) - 2, 3):
                self.tris.append((base + idx[q], base + idx[q + 1], base + idx[q + 2]))
                self.classes.append(cls)
                self.mats.append(mat['name'])
                self.texs.append(tex if U else None)
                self.uvs.append([tuple(U[idx[q + c]][:2]) for c in range(3)] if U else [(0, 0)] * 3)


def build(models):
    """The joined skeleton and the figure's mesh (PARTS)."""
    skel = Skeleton()
    parts = []
    for name, keep in PARTS:
        for m in load(models, name)['models']:
            if m['skeleton']['rotation'] != 'euler_xyz':
                raise ValueError(f'{name}: skeleton rotation {m["skeleton"]["rotation"]} not handled')
            skel.add(m['skeleton'])
            parts.append((name, keep, m))
    mesh = Mesh()
    for name, keep, m in parts:
        mesh.add(m, keep, [skel.index[b['name']] for b in m['skeleton']['bones']])
    return skel, mesh.verts, mesh.weights, mesh.tris, mesh.classes, mesh.uvs, mesh.texs


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


def build_atlas(romfs, names, side=ATLAS, tex_side=256, quiet=False):
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
            w, h, kind, bw, bh, px = txtg.read_txtg(data, tex_side)
        except ValueError as e:
            print(f'  texture {n}: {e}')
            continue
        if px is None:
            print(f'  texture {n}: format {kind} not handled')
            continue
        pics[n] = (w, h, kind, bw, bh, px)
    atlas = bytearray(side * side * 4)
    for y in range(8):
        atlas[y * side * 4:(y * side + 8) * 4] = bytes([255]) * 32
    place, astc = {}, []
    x, y, shelf = 8 + PAD, 0, 8
    for n in sorted(pics, key=lambda k: -pics[k][1]):
        w, h, kind, bw, bh, px = pics[n]
        if x + w > side:
            x, y, shelf = 0, y + shelf + PAD, 0
        if y + h > side:
            if not quiet:
                print(f'  texture {n}: no room in the atlas')
            continue
        place[n] = (x, y, w, h)
        if kind == 'astc':
            astc.append((x, y, w, h, bw | (0x100 if n in OVER_SKIN else 0), bh, px))
        else:
            for r in range(h):
                atlas[((y + r) * side + x) * 4:((y + r) * side + x + w) * 4] = px[r * w * 4:(r + 1) * w * 4]
        x += w + PAD
        shelf = max(shelf, h)
    return place, atlas, astc


def atlas_uv(rect, tri_uv, side=ATLAS):
    """A triangle's corners in the atlas: shifted by whole tiles to start in 0..1, then clamped into the texture with a
    half-texel inset."""
    x, y, w, h = rect
    fu = math.floor(min(u for u, _ in tri_uv))
    fv = math.floor(min(v for _, v in tri_uv))
    out = []
    for u, v in tri_uv:
        u = min(max(u - fu, 0.5 / w), 1 - 0.5 / w)
        v = min(max(v - fv, 0.5 / h), 1 - 0.5 / h)
        out += [(x + u * w) / side, (y + v * h) / side]
    return out


def write(path, verts, weights, tris, classes, uvs, texs, place, atlas, astc, skel, anims, palette=()):
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
    out += b'PALM' + struct.pack('<I', len(palette))
    for bone, rigid in palette:
        out += struct.pack('<HBB', 0xffff if bone is None else bone, 1 if rigid else 0, 0)
    Path(path).write_bytes(out)


def body_palette(models, skel):
    """Link_RaulSkin's skinning palette as the figure's bones: per matrix_to_bone entry (figure bone, rigid)."""
    sk = load(models, PARTS[0][0])['models'][0]['skeleton']
    return [(skel.index.get(sk['bones'][b]['name']), k >= sk['smooth_count']) for k, b in enumerate(sk['matrix_to_bone'])]


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


def zstd_dicts(romfs):
    """{id: dictionary} of the RomFS's zstd dictionaries (Pack/ZsDic.pack.zs)."""
    dicts = {}
    for blob in sarc_members(txtg.unzstd(romfs.read('Pack/ZsDic.pack.zs'))).values():
        if blob[:4] == b'\x37\xa4\x30\xec':   # a zstd dictionary: its id follows the magic
            dicts[struct.unpack_from('<I', blob, 4)[0]] = txtg.zstd_dict(blob)
    return dicts


def unzstd_file(romfs, dicts, path):
    frame = romfs.read(path)
    if frame is None:
        raise FileNotFoundError(f'{path} is not in the RomFS')
    # the frame header names its dictionary: a 1-, 2- or 4-byte id after the descriptor (and window byte)
    fhd = frame[4]
    did_size = (0, 1, 2, 4)[fhd & 3]
    at = 5 + (0 if fhd & 0x20 else 1)
    did = int.from_bytes(frame[at:at + did_size], 'little') if did_size else 0
    return txtg.unzstd(frame, dicts.get(did))


# The armor library (link-armor.bin): Link's bare body and every armor model a worn piece can show, each with its own
# small atlas, and which model each piece (the actor the game names, Armor_002_Upper) shows. The body's parts:
BODY = [('Link.Link_RaulSkin', None), ('Link.Link_RaulSkin_RightHand', None)]
ARMOR_ATLAS = 512   # each model's atlas side; its textures at most 256 (txtg.read_txtg)
BODY_ATLAS = 1024   # the body's: the face, the eyes and the arm's patterns at full size


def groups_of(doc):
    """[(group, [material names])] of a HiddenMaterialGroupList."""
    return [(g.get('GroupName', ''), list(g.get('MaterialNameList', []))) for g in doc.get('HiddenMaterialGroupList', [])
            if g.get('GroupName')]


def body_groups(romfs, dicts):
    """The material groups of Link's own model (the Player's EquipmentUserParam): G_Upper the chest's skin, G_Belt_A
    a belt, G_Head the hair and so on, which the armor he wears hides by name."""
    from achievements import read_byml
    for k, v in sarc_members(unzstd_file(romfs, dicts, 'Pack/Actor/Player.pack.zs')).items():
        if '/EquipmentUserParam/' in k:
            return groups_of(read_byml(v))
    return []


def armor_params(romfs, dicts):
    """{actor: (hides, groups)} of every armor piece: the groups it hides (HideMaterialGroupNameList) and the groups of
    its own model's materials (HiddenMaterialGroupList), from its pack's ArmorParam."""
    from achievements import read_byml
    out = {}
    for path in sorted(romfs.idx):
        m = re.match(r'Pack/Actor/(Armor_[A-Za-z0-9]+_(Head|Upper|Lower))\.pack\.zs$', path)
        if not m:
            continue
        for k, v in sarc_members(unzstd_file(romfs, dicts, path)).items():
            if '/ArmorParam/' in k:
                doc = read_byml(v)
                out[m.group(1)] = (list(doc.get('HideMaterialGroupNameList', [])), groups_of(doc))
    return out


def armor_actors(romfs, dicts):
    """{actor: model file name} of every armor piece (Pack/Actor/Armor_*_{Head,Upper,Lower}): its pack's ModelInfo
    (the one of its own name, else one of its part; a dyed piece points at the model it dyes) names the model as
    ModelProjectName.FmdbName."""
    from achievements import read_byml
    out = {}
    for path in sorted(romfs.idx):
        m = re.match(r'Pack/Actor/(Armor_[A-Za-z0-9]+_(Head|Upper|Lower))\.pack\.zs$', path)
        if not m:
            continue
        actor, part = m.group(1), m.group(2)
        infos = {k.split('/')[-1].split('.')[0]: v for k, v in sarc_members(unzstd_file(romfs, dicts, path)).items()
                 if '/ModelInfo/' in k}
        doc = infos.get(actor) or next((v for k, v in infos.items() if k.endswith('_' + part)), None)
        info = read_byml(doc) if doc else {}
        if info.get('ModelProjectName') and info.get('FmdbName'):
            out[actor] = f"{info['ModelProjectName']}.{info['FmdbName']}"
    return out


def pstr(t):
    b = t.encode()[:255]
    return bytes([len(b)]) + b


def pgroups(groups):
    out = bytearray([len(groups[:255])])
    for name, mats in groups[:255]:
        out += pstr(name) + bytes([len(mats[:255])]) + b''.join(pstr(x) for x in mats[:255])
    return bytes(out)


def build_armor(models, romfs, skel, actors, out_path, params=None, body=()):
    """Writes the armor library: the body (BODY) and each model actors name, skinned to the figure's skeleton by bone
    name (a bone the figure lacks, a cape's or a tassel's, moves with its nearest ancestor the figure has)."""
    from compression import zstd
    entries = [('Link_Body', BODY)] + [(n, [(n, None)]) for n in sorted(set(actors.values()))]
    blobs, index = [], {}
    for name, parts in entries:
        mesh = Mesh()
        try:
            for part, keep in parts:
                for m in load(models, part)['models']:
                    bones = m['skeleton']['bones']
                    joined = []
                    for b in bones:
                        k = b
                        while k['name'] not in skel.index and k['parent'] >= 0:
                            k = bones[k['parent']]
                        joined.append(skel.index.get(k['name'], 0))
                    mesh.add(m, keep, joined)
        except (FileNotFoundError, ValueError) as e:
            print(f'  armor {name}: {e}')
            continue
        names = sorted({x for x in mesh.texs if x})
        side = BODY_ATLAS if name == 'Link_Body' else ARMOR_ATLAS
        for tex_side in (256, 128, 64):   # the largest textures that all fit the model's atlas
            place, atlas, astc = build_atlas(romfs, names, side, tex_side, quiet=tex_side > 64)
            if len(place) == len(names) or tex_side == 64:
                break
        out = bytearray(name.encode()[:47].ljust(48, b'\0'))
        packed = zstd.compress(bytes(atlas))
        out += struct.pack('<IIIII', len(mesh.verts), len(mesh.tris), side, len(astc), len(packed))
        mats = sorted(set(mesh.mats))
        out += struct.pack('<H', len(mats)) + b''.join(pstr(x) for x in mats)
        for v in mesh.verts:
            out += struct.pack('<3f', *v)
        for ws in mesh.weights:
            ws = (ws + [(0, 0.0)] * 4)[:4]
            q = [round(w * 65535) for _, w in ws]
            q[0] += 65535 - sum(q)
            for (bn, _), w in zip(ws, q):
                out += struct.pack('<HH', bn, w)
        for t in mesh.tris:
            out += struct.pack('<3I', *t)
        coords = bytearray()
        for i in range(len(mesh.tris)):
            rect = place.get(mesh.texs[i]) if mesh.texs[i] else None
            out.append(mesh.classes[i] | (0 if rect else 0x80))
            coords += struct.pack('<6f', *(atlas_uv(rect, mesh.uvs[i], side) if rect else [4 / side] * 6))
        out += struct.pack(f'<{len(mesh.tris)}H', *(mats.index(x) for x in mesh.mats))
        out += coords + packed
        for x, y, w, h, bw, bh, blocks in astc:
            out += struct.pack('<7I', x, y, w, h, bw, bh, len(blocks)) + blocks
        index[name] = len(blobs)
        blobs.append(bytes(out))
    params = params or {}
    head = bytearray(b'LNKA') + struct.pack('<III', 2, len(blobs), len(actors)) + pgroups(list(body))
    for actor in sorted(actors):
        hides, groups = params.get(actor, ([], []))
        head += actor.encode()[:47].ljust(48, b'\0') + struct.pack('<i', index.get(actors[actor], -1))
        head += bytes([len(hides[:255])]) + b''.join(pstr(x) for x in hides[:255]) + pgroups(groups)
    Path(out_path).write_bytes(bytes(head) + b''.join(blobs))
    print(f'{len(blobs)} armor models for {len(actors)} pieces -> {out_path} ({(len(head) + sum(map(len, blobs))) >> 20} MiB)')


def prepare(romfs, mcsharp, work, armor=False):
    """The models folder build and main read, made in work from the player's own RomFS: each part's
    Model/<name>.bfres.mc through McSharp's decoder, and the animations unpacked from their zstd frame."""
    import subprocess
    work.mkdir(parents=True, exist_ok=True)
    dicts = zstd_dicts(romfs)
    names = [name for name, _ in PARTS]
    if armor:   # every model a worn piece can show, and the body's parts
        names += [n for n, _ in BODY] + sorted(set(armor_actors(romfs, dicts).values()))
    for name in dict.fromkeys(names):
        data = romfs.read(f'Model/{name}.bfres.mc')
        if data is None:
            if name in dict(PARTS):
                raise FileNotFoundError(f'Model/{name}.bfres.mc is not in the RomFS')
            print(f'  armor {name}: not in the RomFS')
            continue
        (work / f'{name}.bfres.mc').write_bytes(data)
    r = subprocess.run([str(mcsharp), str(work), str(work)], capture_output=True, text=True,
                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if r.returncode:
        raise RuntimeError(f'mcsharp exit {r.returncode}: {r.stdout[-800:]} {r.stderr[-800:]}')
    (work / 'Player_Animation.anim.bfres').write_bytes(unzstd_file(romfs, dicts, 'Model/Player_Animation.anim.bfres.zs'))
    return work


def main(argv):
    armor_out = None
    if '--armor' in argv:   # --armor <out.bin>: also the armor library
        i = argv.index('--armor')
        armor_out = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    if len(argv) > 1 and argv[1] == '--mcsharp':   # --mcsharp <exe> --work <dir> <romfs dir> <out.bin>
        if len(argv) < 7 or argv[3] != '--work':
            sys.exit(__doc__)
        romfs = RomFS(argv[5])
        models = prepare(romfs, Path(argv[2]), Path(argv[4]), armor=armor_out is not None)
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
    write(argv[3], verts, weights, tris, classes, uvs, texs, place, atlas, astc, skel, anims, body_palette(Path(argv[1]), skel))
    print(f'{len(verts)} vertices, {len(tris)} triangles, {len(skel.names)} bones, {len(place)} textures '
          f'({len(astc)} ASTC), {len(anims)} animations -> {argv[3]}')
    if armor_out:
        dicts = zstd_dicts(romfs)
        build_armor(Path(argv[1]), romfs, skel, armor_actors(romfs, dicts), armor_out, armor_params(romfs, dicts),
                    body_groups(romfs, dicts))
    if len(argv) > 4:
        preview(argv[4], verts, tris, classes)


if __name__ == '__main__':
    main(sys.argv)
