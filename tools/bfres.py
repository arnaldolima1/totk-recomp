"""A reader for the BFRES v10 models of Tears of the Kingdom (after MeshCodec decompression): skeleton, shapes with
their vertex attributes and indices, and material texture names. Enough to draw a model and pose it; no writing.

The layouts follow KillzXGaming/BfresLibrary (MIT, Copyright (c) KillzXGaming; commit e2c5deb), read for the order and
size of each structure's fields: Switch/ResFileParser.cs, Switch/Model/*Parser*.cs, Shared/Models/*.cs.

    python tools/bfres.py <file.bfres>    prints a summary

Every structure is little-endian. Offsets are 64-bit and absolute. Strings are a u16 length then the characters,
zero-terminated. A dictionary is u32 0, i32 count, then count+1 nodes of 16 bytes (u32 ref, u16 left, u16 right, u64
key). Version 10 files that hold their GPU data externally keep it right after the file's declared size, 288 bytes on.
"""
import struct
import sys
from pathlib import Path


class Reader:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def u8(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def u16(self):
        v = struct.unpack_from('<H', self.d, self.p)[0]
        self.p += 2
        return v

    def i16(self):
        v = struct.unpack_from('<h', self.d, self.p)[0]
        self.p += 2
        return v

    def u32(self):
        v = struct.unpack_from('<I', self.d, self.p)[0]
        self.p += 4
        return v

    def u64(self):
        v = struct.unpack_from('<Q', self.d, self.p)[0]
        self.p += 8
        return v

    def f32s(self, n):
        v = struct.unpack_from(f'<{n}f', self.d, self.p)
        self.p += 4 * n
        return list(v)

    def skip(self, n):
        self.p += n

    def sig(self, s):
        got = self.d[self.p:self.p + 4]
        if got != s.encode():
            raise ValueError(f'expected {s} at {self.p:#x}, found {got!r}')
        self.p += 4

    def string_at(self, off):
        if not off or off >= len(self.d):
            return ''
        end = self.d.index(b'\0', off + 2)
        return self.d[off + 2:end].decode('utf-8', 'replace')

    def string(self):
        return self.string_at(self.u64())

    def dict_keys(self, off):
        """The names of a dictionary, in value order (node 0 is the root and has no value)."""
        if not off:
            return []
        n = struct.unpack_from('<i', self.d, off + 4)[0]
        return [self.string_at(struct.unpack_from('<Q', self.d, off + 8 + 16 * i + 8)[0]) for i in range(1, n + 1)]

    def at(self, off):
        r = Reader(self.d)
        r.p = off
        return r


# Vertex attribute formats: the code's high byte is the kind, the low byte the layout. layout: (components, bits).
LAYOUTS = {0x02: (1, 8), 0x09: (2, 8), 0x0B: (4, 8), 0x0A: (1, 16), 0x12: (2, 16), 0x15: (4, 16), 0x14: (1, 32),
           0x16: (1, 32), 0x17: (2, 32), 0x18: (3, 32), 0x19: (4, 32)}
KINDS = {1: 'unorm', 2: 'snorm', 3: 'uint', 4: 'sint', 5: 'float'}


def decode_attrib(fmt, buf, offset, stride, count):
    """The attribute's values as lists of floats (ints for integer kinds), one list per vertex."""
    kind, layout = fmt >> 8, fmt & 0xFF
    if fmt == 0x020E or fmt == 0x000B:   # 10_10_10_2, signed or unsigned normalized (normals, tangents)
        out = []
        for i in range(count):
            w = struct.unpack_from('<I', buf, offset + i * stride)[0]
            comps = []
            for k in range(3):
                c = (w >> (10 * k)) & 0x3FF
                if fmt == 0x020E:
                    c = c - 0x400 if c & 0x200 else c
                    comps.append(max(c / 511.0, -1.0))
                else:
                    comps.append(c / 1023.0)
            a = w >> 30
            comps.append((a - 4 if fmt == 0x020E and a & 2 else a) / (1.0 if fmt == 0x020E else 3.0))
            out.append(comps)
        return out
    if layout not in LAYOUTS or kind not in KINDS:
        raise ValueError(f'vertex format {fmt:#06x} not handled')
    n, bits = LAYOUTS[layout]
    k = KINDS[kind]
    code = {(8, 'unorm'): 'B', (8, 'uint'): 'B', (8, 'snorm'): 'b', (8, 'sint'): 'b', (16, 'unorm'): 'H',
            (16, 'uint'): 'H', (16, 'snorm'): 'h', (16, 'sint'): 'h', (16, 'float'): 'e', (32, 'uint'): 'I',
            (32, 'sint'): 'i', (32, 'float'): 'f'}[(bits, k)]
    scale = {'unorm': (1 << bits) - 1, 'snorm': (1 << (bits - 1)) - 1}.get(k)
    out = []
    for i in range(count):
        v = struct.unpack_from('<' + code * n, buf, offset + i * stride)
        out.append([max(x / scale, -1.0) if scale else x for x in v])
    return out


def read_bfres(data):
    r = Reader(data)
    r.sig('FRES')
    r.skip(4)
    version = r.u32()
    if version >> 16 != 10:
        raise ValueError(f'BFRES version {version:#x}: this reader knows version 10 (Tears of the Kingdom)')
    r.skip(4)                                 # byte order, alignment, address size
    r.skip(4)                                 # file name offset
    r.skip(4)                                 # flags, block offset
    r.skip(4)                                 # relocation table
    file_size = r.u32()
    name = r.string()
    model_off, model_dict = r.u64(), r.u64()
    ext_flags = data[0xEE]
    gpu_base = file_size + 288 if ext_flags & 0x02 else None   # HasExternalGPU
    if gpu_base is None:   # the memory pool's buffer info: the GPU data inside the file
        r.skip(32 + 16 * 5)   # reserved, the five animation dictionaries
        r.skip(8)             # memory pool
        info = r.u64()
        gpu_base = struct.unpack_from('<I', data, info + 4)[0] if info else 0
    models = []
    names = r.dict_keys(model_dict)
    m = r.at(model_off)
    for mname in names:
        models.append(read_model(m, data, gpu_base))
    return {'name': name, 'models': models}


def read_model(r, data, gpu):
    start = r.p
    r.sig('FMDL')
    r.u32()
    name, path = r.string(), r.string()
    skel_off = r.u64()
    vtx_off = r.u64()
    shape_vals, shape_dict = r.u64(), r.u64()
    mat_vals, mat_dict = r.u64(), r.u64()
    r.u64()                                   # shader assigns
    r.skip(16 + 8)                            # user data, user pointer
    n_vtx, n_shape, n_mat = r.u16(), r.u16(), r.u16()
    r.skip(2 + 2 + 2 + 4)
    skeleton = read_skeleton(r.at(skel_off))
    vbufs = []
    vr = r.at(vtx_off)
    for _ in range(n_vtx):
        vbufs.append(read_vertex_buffer(vr, data, gpu))
    shapes = []
    sr = r.at(shape_vals)
    for sname in r.dict_keys(shape_dict):
        shapes.append(read_shape(sr, data, gpu, vbufs))
    materials = []
    mr = r.at(mat_vals)
    for _ in r.dict_keys(mat_dict):
        materials.append(read_material(mr))
    return {'name': name, 'skeleton': skeleton, 'shapes': shapes, 'materials': materials}


def read_skeleton(r):
    r.sig('FSKL')
    flags = r.u32()
    bone_dict, bone_vals = r.u64(), r.u64()
    m2b_off, inv_off = r.u64(), r.u64()
    r.skip(8)                                 # user pointer
    r.skip(8)                                 # mirror table
    n_bone, n_smooth, n_rigid = r.u16(), r.u16(), r.u16()
    bones = []
    br = r.at(bone_vals)
    for _ in range(n_bone):
        bname = br.string()
        br.skip(16 + 8)                       # user data, reserved
        br.u16()
        parent, smooth, rigid = br.i16(), br.i16(), br.i16()
        br.skip(2 + 2)                        # billboard, user data count
        bflags = br.u32()
        scale, rot, pos = br.f32s(3), br.f32s(4), br.f32s(3)
        bones.append({'name': bname, 'parent': parent, 'smooth': smooth, 'rigid': rigid, 'scale': scale,
                      'rot': rot, 'pos': pos, 'flags': bflags})
    m2b = list(struct.unpack_from(f'<{n_smooth + n_rigid}H', r.d, m2b_off)) if m2b_off else []
    inv = [list(struct.unpack_from('<12f', r.d, inv_off + 48 * i)) for i in range(n_smooth)] if inv_off else []
    return {'bones': bones, 'matrix_to_bone': m2b, 'inverse': inv, 'smooth_count': n_smooth,
            'rotation': 'quaternion' if (flags >> 12) & 7 == 0 else 'euler_xyz'}


def read_vertex_buffer(r, data, gpu):
    r.sig('FVTX')
    r.u32()
    attr_vals, attr_dict = r.u64(), r.u64()
    r.skip(8 + 8 + 8)                         # memory pool, two unknown offsets
    size_off, stride_off = r.u64(), r.u64()
    r.skip(8)
    buf_off = r.u32()
    n_attr, n_buf = r.u8(), r.u8()
    r.u16()
    count = r.u32()
    skin = r.u16()
    align = r.u16() or 1
    sizes = [struct.unpack_from('<I', data, size_off + 16 * i)[0] for i in range(n_buf)]
    strides = [struct.unpack_from('<I', data, stride_off + 16 * i)[0] for i in range(n_buf)]
    bufs, p = [], gpu + buf_off
    for i in range(n_buf):
        p = (p + align - 1) // align * align
        bufs.append(data[p:p + sizes[i]])
        p += sizes[i]
    attrs = {}
    ar = r.at(attr_vals)
    for aname in r.dict_keys(attr_dict):
        nm = ar.string()
        fmt = struct.unpack_from('>H', data, ar.p)[0]
        ar.skip(2 + 2)
        off, bi = ar.u16(), ar.u16()
        attrs[nm] = decode_attrib(fmt, bufs[bi], off, strides[bi], count)
    return {'count': count, 'skin': skin, 'attrs': attrs}


def read_shape(r, data, gpu, vbufs):
    r.sig('FSHP')
    r.u32()
    name = r.string()
    r.u64()                                   # its vertex buffer (also by index below)
    mesh_off, skin_off = r.u64(), r.u64()
    r.skip(16)                                # key shapes
    r.skip(8 + 8 + 8)                         # bounding boxes, radii, user pointer
    r.u16()
    mat, bone, vb = r.u16(), r.u16(), r.u16()
    n_skin_bones = r.u16()
    skin, n_mesh = r.u8(), r.u8()
    r.skip(1 + 1 + 2)
    skin_bones = list(struct.unpack_from(f'<{n_skin_bones}H', data, skin_off)) if n_skin_bones else []
    meshes = []
    mr = r.at(mesh_off)
    for _ in range(n_mesh):               # level of detail 0 first
        mr.u64()                          # submeshes
        mr.u64()                          # memory pool
        mr.u64()                          # buffer
        size_off = mr.u64()
        face_off = mr.u32()
        prim, ifmt = mr.u32(), mr.u32()
        n_idx = mr.u32()
        first = mr.u32()
        mr.skip(4)
        isize = struct.unpack_from('<I', data, size_off)[0]
        code = {0: 'B', 1: 'H', 2: 'I'}[ifmt]
        idx = list(struct.unpack_from(f'<{n_idx}{code}', data, gpu + face_off))
        meshes.append({'primitive': prim, 'indices': idx, 'first_vertex': first, 'bytes': isize})
    return {'name': name, 'material': mat, 'bone': bone, 'vertex_buffer': vbufs[vb], 'skin': skin,
            'skin_bones': skin_bones, 'meshes': meshes}


def read_material(r):
    r.sig('FMAT')
    r.u32()
    name = r.string()
    r.u64()                                   # shader info
    r.u64()                                   # texture array
    names_off = r.u64()
    r.skip(8 + 16)                            # sampler array, samplers
    r.skip(8 * 5 + 8 + 16 + 8 * 4)            # render info, source params, reserved, user data, slots
    r.u16()
    n_sampler, n_tex = r.u8(), r.u8()
    r.skip(2 + 2 + 2 + 2 + 4)
    tex = [r.string_at(struct.unpack_from('<Q', r.d, names_off + 8 * i)[0]) for i in range(n_tex)] if names_off else []
    return {'name': name, 'textures': tex}


# ---- skeletal animations (FSKA)

def _curve(r, data):
    frames_off, keys_off = r.u64(), r.u64()
    flags, n = r.u16(), r.u16()
    target = r.u32()
    start, end, scale, offset, delta = r.f32s(5)
    r.skip(4)
    ftype, ktype, ctype = flags & 3, (flags >> 2) & 3, (flags >> 4) & 7
    if ftype == 0:
        frames = list(struct.unpack_from(f'<{n}f', data, frames_off))
    elif ftype == 1:   # 10.5 fixed point
        frames = [v / 32.0 for v in struct.unpack_from(f'<{n}h', data, frames_off)]
    else:
        frames = list(data[frames_off:frames_off + n])
    per = 4 if ctype == 0 else 2 if ctype == 1 else 1
    code = {0: 'f', 1: 'h', 2: 'b'}[ktype]
    keys = list(struct.unpack_from(f'<{n * per}{code}', data, keys_off))
    keys = [keys[i * per:(i + 1) * per] for i in range(n)]
    return {'target': target, 'frames': frames, 'keys': keys, 'type': ctype, 'scale': scale, 'offset': offset}


def curve_at(c, f):
    """The curve's value at frame f: cubic keys are polynomial coefficients over the segment (t 0..1), linear keys a
    value and its delta, baked keys one value a frame; value * scale + offset."""
    fr, k = c['frames'], c['keys']
    if not fr:
        return c['offset']
    if f <= fr[0]:
        i, t = 0, 0.0
    elif f >= fr[-1]:
        i, t = len(fr) - 1, 0.0
    else:
        i = max(j for j in range(len(fr)) if fr[j] <= f)
        span = fr[i + 1] - fr[i]
        t = (f - fr[i]) / span if span else 0.0
    kk = k[i]
    if c['type'] == 0:
        v = kk[0] + kk[1] * t + kk[2] * t * t + kk[3] * t * t * t
    elif c['type'] == 1:
        v = kk[0] + kk[1] * t
    else:
        v = kk[0]
    return v * c['scale'] + c['offset']


def read_skeletal_anims(data, wanted):
    """{name: animation} for the skeletal animations named in wanted. An animation: frame count, euler or quaternion,
    and per bone (by name) its base scale / rotation / translation and the curves animating them (by data offset:
    scale 0x04.., translation 0x10.., rotation 0x20..)."""
    r = Reader(data)
    r.sig('FRES')
    r.skip(4 + 4 + 4 + 4 + 4 + 4 + 4)
    r.string()
    r.skip(16 + 32)
    vals, dct = r.u64(), r.u64()
    names = r.dict_keys(dct)
    return {name: _read_fska(r, data, vals, i) for i, name in enumerate(names) if name in wanted}


def _fska_at(r, data, off):
    a = r.at(off)
    a.sig('FSKA')
    flags = a.u32()
    name, path = a.string(), a.string()
    a.u64()                                   # bind skeleton
    bind_idx, bone_off = a.u64(), a.u64()
    a.skip(16)                                # user data
    frames = struct.unpack_from('<i', data, a.p)[0]
    a.skip(4)
    a.skip(4 + 4)                             # curve count, baked size
    n_bone = a.u16()
    a.skip(2)
    return a, flags, name, frames, bone_off, n_bone


FSKA_SIZE = 0x50   # one animation header; they follow each other in dictionary order


def _read_fska(r, data, vals, i):
    a, flags, name, frames, bone_off, n_bone = _fska_at(r, data, vals + FSKA_SIZE * i)
    bones = {}
    b = r.at(bone_off)
    for _ in range(n_bone):
        bname = b.string()
        curve_off, base_off = b.u64(), b.u64()
        b.skip(16)
        bflags = b.u32()
        b.skip(1 + 1)
        n_curve = b.u8()
        b.skip(1 + 4 + 4)
        base = {}
        p = base_off
        if bflags & (1 << 3):
            base['scale'] = list(struct.unpack_from('<3f', data, p)); p += 12
        if bflags & (1 << 4):
            base['rot'] = list(struct.unpack_from('<4f', data, p)); p += 16
        if bflags & (1 << 5):
            base['pos'] = list(struct.unpack_from('<3f', data, p)); p += 12
        c = r.at(curve_off)
        curves = [_curve(c, data) for _ in range(n_curve)]
        bones[bname] = {'base': base, 'curves': curves}
    return {'name': name, 'frames': frames, 'quat': ((flags >> 12) & 7) == 0, 'bones': bones}


def main(argv):
    f = read_bfres(Path(argv[1]).read_bytes())
    for m in f['models']:
        sk = m['skeleton']
        print(f"model {m['name']}: {len(sk['bones'])} bones ({sk['rotation']}), {len(m['shapes'])} shapes, "
              f"{len(m['materials'])} materials")
        for s in m['shapes']:
            vb = s['vertex_buffer']
            print(f"  shape {s['name']}: material {m['materials'][s['material']]['name']}, {vb['count']} vertices, "
                  f"skin {s['skin']}, attributes {sorted(vb['attrs'])}, LOD0 {len(s['meshes'][0]['indices'])} indices")
        for mat in m['materials']:
            print(f"  material {mat['name']}: {mat['textures']}")


if __name__ == '__main__':
    main(sys.argv)
