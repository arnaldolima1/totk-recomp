"""RomFS mods for an installed TotK Recomp folder, run by the launcher with the release's Python (3.14: compression.zstd).

Mods are folders in <install>/mods/, each with a romfs/ folder (the layout emulator mods use) somewhere in its first two
levels. The runtime (runtime/romfs.c overlay_load) takes one overlay folder, TOTK_ROMFS_MODS, whose manifest.tsv lists
every file with its size, its SHA-256 and the SHA-256 of the original it replaces ("-" for a file the RomFS lacks), and
stops the game when two overlays replace the same file. So the enabled mods are merged into <install>/mods/.merged/
(hard links, copies across disks), earlier mods in the list winning a file both replace, with the originals hashed from
romfs/romfs.bin through romfs-index.bin (tools/romfs_index.py, TOTKRFS1 layout).

The 60 fps overlay (<install>/romfs-patches, tools/make_romfs_patches.py) edits one file, the Common layout archive. A
mod that replaces that archive gets the same edit applied to its own copy (the functions below are those of
tools/make_romfs_patches.py, adapted from MacNativeGames games/totk/scripts/make_romfs_patches.py @ ebe621d, MIT,
Copyright (c) 2026 MacNativeGames contributors); the merged folder then carries every file of romfs-patches too and the
launcher sets romfs_patches = 0, so no file is replaced twice.

    python mods.py scan  --home DIR                 one JSON object: the mods and what they replace
    python mods.py build --home DIR --mods A B ...  JSON lines: progress, then the result

Standard library only. Writes only under <install>/mods/.merged.
"""
import argparse
import compression.zstd as zstd
import hashlib
import json
import os
import re
import shutil
import struct
import sys
from pathlib import Path

MAGIC = b'TOTKRFS1'
HEADER = struct.Struct('<8sQQ')
ROW = struct.Struct('<QQII')
MERGED = '.merged'
COMMON = {'1.2.1': '110', '1.4.3': '140'}   # tools/make_romfs_patches.py COMMON
MEMBERS = {
    'anim/ShortCutBG_00_In.bflan': ('C_CaptureBG_00', 'C_CaptureClearBG_00', 'P_CaptureClearBG_00'),
    'anim/AutoBuilder_00_In.bflan': ('C_CaptureBG_00',),
}
# The game version a versioned file name was made for: Product.121 (text, size table) and Product.110 (layouts) are
# 1.2.1's, Product.140 / 141 / 142 / 143 1.4.x's.
VERSION_OF = {'121': '1.2.1', '120': '1.2.1', '110': '1.2.1', '140': '1.4.3', '141': '1.4.3', '142': '1.4.3', '143': '1.4.3'}


def emit(**e):
    print(json.dumps(e, ensure_ascii=False), flush=True)


def read_index(home):
    data = (home / 'romfs-index.bin').read_bytes()
    magic, count, strings = HEADER.unpack_from(data, 0)
    if magic != MAGIC:
        raise SystemExit('romfs-index.bin: unknown layout')
    out = {}
    for i in range(count):
        offset, size, name_off, name_len = ROW.unpack_from(data, HEADER.size + i * ROW.size)
        name = data[strings + name_off:strings + name_off + name_len - 1].decode()
        out[name] = (offset, size)
    return out


def romfs_root(mod):
    """The romfs/ folder of a mod: itself named romfs, or within two levels."""
    for p in [mod, *sorted(mod.iterdir())] if mod.is_dir() else []:
        if p.is_dir() and p.name.lower() == 'romfs':
            return p
    for sub in sorted(p for p in mod.iterdir() if p.is_dir()):
        for p in sorted(sub.iterdir()):
            if p.is_dir() and p.name.lower() == 'romfs':
                return p
    return None


def mod_files(root):
    """RomFS path -> file, every file under root."""
    out = {}
    for dirpath, _, names in os.walk(root):
        for n in names:
            f = Path(dirpath) / n
            out[f.relative_to(root).as_posix()] = f
    return out


def made_for(files, index):
    """The game versions the mod's versioned file names point at that the RomFS lacks (it was made for another
    version), from names like USen.Product.121.sarc.zs."""
    other = set()
    for path in files:
        m = re.search(r'\.Product\.(\d{3})\.', path)
        if m and path not in index and m.group(1) in VERSION_OF:
            other.add(VERSION_OF[m.group(1)])
    return sorted(other)


def installed_version(home):
    try:
        return json.loads((home / 'totk-install.json').read_text(encoding='utf-8')).get('version')
    except (OSError, ValueError):
        return None


def scan(home):
    index = read_index(home)
    version = installed_version(home)
    mods = []
    folder = home / 'mods'
    for mod in sorted(folder.iterdir()) if folder.is_dir() else []:
        if not mod.is_dir() or mod.name.startswith('.'):
            continue
        root = romfs_root(mod)
        entry = {'name': mod.name, 'romfs': bool(root), 'exefs': any(p.name.lower() == 'exefs' for p in mod.rglob('*') if p.is_dir())}
        if root:
            files = mod_files(root)
            entry.update(files=len(files), size=sum(f.stat().st_size for f in files.values()),
                         replaces=sum(p in index for p in files), adds=sum(p not in index for p in files),
                         made_for=made_for(files, index))
            areas = {}
            for p in files:
                top = p.split('/')[0]
                areas[top] = areas.get(top, 0) + 1
            entry['areas'] = areas
        mods.append(entry)
    print(json.dumps({'version': version, 'mods': mods}, ensure_ascii=False))


def sha_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def sarc_members(data):
    """name -> (absolute start, end) of every SARC member."""
    assert data[:4] == b'SARC' and data[6:8] == b'\xff\xfe'
    header, size, start = struct.unpack_from('<HxxII', data, 4)
    assert size == len(data) and data[header:header + 4] == b'SFAT'
    count = struct.unpack_from('<H', data, header + 6)[0]
    names = header + 12 + count * 16
    assert data[names:names + 4] == b'SFNT'
    names += 8
    out = {}
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from('<4I', data, header + 12 + i * 16)
        offset = names + (attribute & 0xffffff) * 4
        out[data[offset:data.index(0, offset, start)].decode()] = (start + lo, start + hi)
    return out


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


def patch_common(raw, read_original):
    """The 60 fps edit of tools/make_romfs_patches.py applied to a mod's Common archive (raw .zs bytes); None when the
    archive does not have the expected animation keys."""
    pack = zstd.decompress(read_original('Pack/ZsDic.pack.zs'))
    dictionaries = {}
    for lo, hi in sarc_members(pack).values():
        d = zstd.ZstdDict(pack[lo:hi])
        dictionaries[d.dict_id] = d
    info = zstd.get_frame_info(raw)
    dictionary = dictionaries.get(info.dictionary_id)
    blarc = bytearray(zstd.decompress(raw, zstd_dict=dictionary) if dictionary else zstd.decompress(raw))
    members = sarc_members(blarc)
    for member, panes in MEMBERS.items():
        if member not in members:
            return None
        lo, _ = members[member]
        changed = []
        for name, ko, keys in flvi_keys(blarc, lo):
            if name not in panes:
                continue
            if [(f, v) for f, v in keys] != [(0.0, 0), (1.0, 1)]:
                return None
            struct.pack_into('<f', blarc, ko + 8, 0.0)
            changed.append(name)
        if sorted(changed) != sorted(panes):
            return None
    data = zstd.compress(bytes(blarc), level=19, zstd_dict=dictionary) if dictionary else zstd.compress(bytes(blarc), level=19)
    return data


def place(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copyfile(src, dst)


def build(home, names):
    index = read_index(home)
    image = open(home / 'romfs' / 'romfs.bin', 'rb')

    def read_original(path):
        offset, size = index[path]
        image.seek(offset)
        return image.read(size)

    def sha_original(path):
        offset, size = index[path]
        h = hashlib.sha256()
        image.seek(offset)
        left = size
        while left:
            chunk = image.read(min(left, 1 << 20))
            h.update(chunk)
            left -= len(chunk)
        return h.hexdigest()

    out = home / 'mods' / MERGED
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    chosen, conflicts, warnings = {}, [], []
    for name in names:   # earlier mods win
        root = romfs_root(home / 'mods' / name)
        if not root:
            warnings.append({'id': 'no_romfs', 'mod': name})
            continue
        for path, f in mod_files(root).items():
            if path in chosen:
                conflicts.append({'path': path, 'kept': chosen[path][0], 'dropped': name})
            else:
                chosen[path] = (name, f)
    patches = {}   # romfs-patches: path -> its manifest line
    manifest = home / 'romfs-patches' / 'manifest.tsv'
    if manifest.is_file():
        for line in manifest.read_text(encoding='utf-8').splitlines():
            if line and not line.startswith('#'):
                patches[line.split('\t')[0]] = line
    clash = [p for p in chosen if p in patches]
    version = installed_version(home)
    common = f'UI/LayoutArchive/Common.Product.{COMMON.get(version, "")}.Nin_NX_NVN.blarc.zs'
    lines, total, done = [], len(chosen), 0
    for path, (name, f) in sorted(chosen.items()):
        known = path in index
        if path == common and path in patches:
            try:
                data = patch_common(f.read_bytes(), read_original)
            except Exception:   # (not the layout tools/make_romfs_patches.py expects)
                data = None
            if data is None:
                warnings.append({'id': 'common_unpatched', 'mod': name})
                place(f, out / path)
                lines.append([path, str(f.stat().st_size), sha_file(f)])
            else:
                (out / path).parent.mkdir(parents=True, exist_ok=True)
                (out / path).write_bytes(data)
                lines.append([path, str(len(data)), hashlib.sha256(data).hexdigest()])
        else:
            place(f, out / path)
            lines.append([path, str(f.stat().st_size), sha_file(f)])
        lines[-1].append(sha_original(path) if known else '-')
        done += 1
        if done % 50 == 0 or done == total:
            emit(type='progress', done=done, total=total)
    if clash:   # the merged folder carries romfs-patches' other files, and the launcher turns romfs-patches off
        for path, line in patches.items():
            if path not in chosen:
                place(home / 'romfs-patches' / path, out / path)
                lines.append(line.split('\t'))
    (out / 'manifest.tsv').write_text('# path\tsize\tsha256\toriginal_sha256 (TotK Recomp launcher, mods.py)\n'
                                      + ''.join('\t'.join(l) + '\n' for l in lines), encoding='utf-8')
    image.close()
    emit(type='result', dir=str(out), files=len(lines), conflicts=conflicts, warnings=warnings,
         patches_merged=bool(clash))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', choices=('scan', 'build'))
    ap.add_argument('--home', type=Path, required=True)
    ap.add_argument('--mods', nargs='*', default=[])
    a = ap.parse_args()
    try:
        scan(a.home) if a.mode == 'scan' else build(a.home, a.mods)
    except Exception as e:   # one line the launcher shows
        emit(type='error', message=f'{type(e).__name__}: {e}')
        sys.exit(1)


main()
