"""Adapt a RomFS mod made for another TotK version (a translation made for 1.2.1, say) to the installed game, run by the
launcher with the release's Python (3.14: compression.zstd).

    python adapt.py --home <install> --src <mod folder or .zip> --out <new mod folder> [--zip <file.zip>]

The installed game is read through romfs-index.bin and romfs/romfs.bin (tools/romfs_index.py, TOTKRFS1 layout). Every
file of the mod's romfs/ is handled by what it is:

- a file the installed RomFS has under the same name (voices, sounds, textures): copied;
- a versioned file the installed RomFS has under another version number (Product.121 -> Product.142):
  - Mals/<lang>.Product.<v>.sarc.zs, the game text: the installed archive with each message the mod has replaced by
    the mod's (matched by MSBT label), so messages the newer version added stay in its language;
  - System/Resource/ResourceSizeTable.Product.<v>....rsizetable.zs: rebuilt last from the installed table, every file
    of the adapted mod getting room for its size (the mod's own value for it, or the installed value grown by the
    file's growth);
  - UI/LayoutArchive/*.blarc.zs: left out with a warning; a layout archive of another version also differs in layouts,
    shaders and icons, so replacing the installed one would break newer menus;
  - anything else versioned: left out with a warning;
- a file only the mod has: added as it is.

Output: one JSON line per event on stdout (progress, warning, result), the adapted mod in --out (romfs/ plus
adapted.json describing what was done) and, with --zip, the same as a zip. Standard library only.
"""
import argparse
import compression.zstd as zstd
import json
import re
import shutil
import struct
import sys
import tempfile
import zipfile
import zlib
from pathlib import Path

VERSIONED = re.compile(r'\.Product\.(\d{3})(\.Nin_NX_NVN)?')


def emit(**e):
    print(json.dumps(e, ensure_ascii=False), flush=True)


# ---------------------------------------------------------------- the installed game

class Game:
    def __init__(self, home):
        data = (home / 'romfs-index.bin').read_bytes()
        magic, count, strings = struct.unpack_from('<8sQQ', data, 0)
        if magic != b'TOTKRFS1':
            raise SystemExit('romfs-index.bin: unknown layout')
        self.index = {}
        for i in range(count):
            off, size, no, nl = struct.unpack_from('<QQII', data, 24 + i * 24)
            self.index[data[strings + no:strings + no + nl - 1].decode()] = (off, size)
        self.image = open(home / 'romfs' / 'romfs.bin', 'rb')
        self.by_unversioned = {}
        for p in self.index:
            self.by_unversioned.setdefault(VERSIONED.sub('.Product.*', p), []).append(p)
        self.dicts = {}
        pack = zstd.decompress(self.read('Pack/ZsDic.pack.zs'))
        for name, b in sarc_read(pack)[0].items():
            d = zstd.ZstdDict(b)
            self.dicts[d.dict_id] = d

    def read(self, path):
        off, size = self.index[path]
        self.image.seek(off)
        return self.image.read(size)

    def counterpart(self, path):
        """The installed file a versioned mod file stands for (another version number), or None."""
        if not VERSIONED.search(path):
            return None
        hits = [p for p in self.by_unversioned.get(VERSIONED.sub('.Product.*', path), []) if p != path]
        return hits[0] if len(hits) == 1 else None

    def decompress(self, raw):
        """(data, dictionary or None) of a .zs file."""
        info = zstd.get_frame_info(raw)
        d = self.dicts.get(info.dictionary_id) if info.dictionary_id else None
        if info.dictionary_id and d is None:
            raise ValueError(f'unknown zstd dictionary {info.dictionary_id:#x}')
        return zstd.decompress(raw, zstd_dict=d) if d else zstd.decompress(raw), d

    def compress(self, data, d):
        return zstd.compress(data, level=19, zstd_dict=d) if d else zstd.compress(data, level=19)


# ---------------------------------------------------------------- SARC

def sarc_read(d):
    """(name -> bytes, layout) of a SARC; layout keeps what sarc_write needs to rebuild it the same way."""
    assert d[:4] == b'SARC' and d[6:8] == b'\xff\xfe', 'not a little-endian SARC'
    header, size, start = struct.unpack_from('<HxxII', d, 4)
    count = struct.unpack_from('<H', d, header + 6)[0]
    names = header + 12 + count * 16 + 8
    members, order = {}, []
    align = 0
    for i in range(count):
        _, attr, lo, hi = struct.unpack_from('<4I', d, header + 12 + i * 16)
        o = names + (attr & 0xffffff) * 4
        name = d[o:d.index(0, o)].decode()
        members[name] = d[start + lo:start + hi]
        order.append(name)
        align |= lo
    alignment = (align & -align) if align else 8
    return members, {'head': d[:start], 'node_table': header + 12, 'order': order, 'alignment': min(alignment, 0x2000)}


def sarc_write(members, layout):
    """The SARC of layout (the same header, nodes and names) with each member's data from members."""
    head = bytearray(layout['head'])
    data = bytearray()
    a = layout['alignment']
    for i, name in enumerate(layout['order']):
        data += b'\0' * ((-len(data)) % a)
        lo = len(data)
        data += members[name]
        struct.pack_into('<II', head, layout['node_table'] + i * 16 + 8, lo, len(data))
    out = head + data
    struct.pack_into('<I', out, 0x08, len(out))   # header: magic, header size, BOM, file size, data offset
    return bytes(out)


# ---------------------------------------------------------------- MSBT

def msbt_sections(b):
    assert b[:8] == b'MsgStdBn', 'not an MSBT'
    assert b[8:10] == b'\xff\xfe', 'big-endian MSBT'
    count = struct.unpack_from('<H', b, 0x0e)[0]
    pos, out = 0x20, []
    for _ in range(count):
        magic, size = b[pos:pos + 4], struct.unpack_from('<I', b, pos + 4)[0]
        out.append((magic, b[pos + 16:pos + 16 + size]))
        pos += 16 + ((size + 15) & ~15)
    return out


def msbt_labels(body):
    groups = struct.unpack_from('<I', body, 0)[0]
    labels = {}
    for g in range(groups):
        cnt, off = struct.unpack_from('<II', body, 4 + g * 8)
        p = off
        for _ in range(cnt):
            n = body[p]
            labels[body[p + 1:p + 1 + n].decode()] = struct.unpack_from('<I', body, p + 1 + n)[0]
            p += 1 + n + 4
    return labels


def msbt_texts(body):
    n = struct.unpack_from('<I', body, 0)[0]
    offs = [struct.unpack_from('<I', body, 4 + 4 * i)[0] for i in range(n)] + [len(body)]
    return [body[offs[i]:offs[i + 1]] for i in range(n)]


def msbt_merge(base, mod):
    """base (the installed MSBT) with the text of every label mod also has taken from mod; (bytes, replaced, kept)."""
    bs, ms = msbt_sections(base), msbt_sections(mod)
    get = lambda secs, m: next((body for magic, body in secs if magic == m), None)
    bl, bt = msbt_labels(get(bs, b'LBL1')), msbt_texts(get(bs, b'TXT2'))
    ml, mt = msbt_labels(get(ms, b'LBL1')), msbt_texts(get(ms, b'TXT2'))
    texts = list(bt)
    replaced = 0
    for label, i in bl.items():
        j = ml.get(label)
        if j is not None and j < len(mt) and texts[i] != mt[j]:
            texts[i] = mt[j]
            replaced += 1
    kept = sum(1 for label in bl if label not in ml)
    if not replaced:
        return base, 0, kept
    body = bytearray(struct.pack('<I', len(texts)))
    off = 4 + 4 * len(texts)
    for t in texts:
        body += struct.pack('<I', off)
        off += len(t)
    for t in texts:
        body += t
    out = bytearray(base[:0x20])
    for magic, sec in bs:
        sec = bytes(body) if magic == b'TXT2' else sec
        out += magic + struct.pack('<I', len(sec)) + b'\0' * 8 + sec
        out += b'\xab' * ((-len(sec)) % 16)
    struct.pack_into('<I', out, 0x12, len(out))
    return bytes(out), replaced, kept


# ---------------------------------------------------------------- RESTBL (ResourceSizeTable)

def restbl_read(d):
    assert d[:6] == b'RESTBL', 'not a RESTBL'
    version, strsize, ncrc, nname = struct.unpack_from('<IIII', d, 6)
    pos, crc, names = 22, {}, {}
    for _ in range(ncrc):
        h, v = struct.unpack_from('<II', d, pos)
        crc[h] = v
        pos += 8
    for _ in range(nname):
        n = d[pos:pos + strsize].split(b'\0')[0].decode()
        names[n] = struct.unpack_from('<I', d, pos + strsize)[0]
        pos += strsize + 4
    return {'version': version, 'strsize': strsize, 'crc': crc, 'names': names}


def restbl_write(t):
    out = bytearray(b'RESTBL' + struct.pack('<IIII', t['version'], t['strsize'], len(t['crc']), len(t['names'])))
    for h in sorted(t['crc']):
        out += struct.pack('<II', h, t['crc'][h])
    for n in sorted(t['names']):
        out += n.encode().ljust(t['strsize'], b'\0') + struct.pack('<I', t['names'][n])
    return bytes(out)


def restbl_get(t, key):
    return t['names'][key] if key in t['names'] else t['crc'].get(zlib.crc32(key.encode()))


def restbl_set(t, key, value):
    if key in t['names']:
        t['names'][key] = value
    else:
        t['crc'][zlib.crc32(key.encode())] = value


# ---------------------------------------------------------------- the mod

def romfs_root(folder):
    for p in [folder, *sorted(folder.iterdir())]:
        if p.is_dir() and p.name.lower() == 'romfs':
            return p
    for sub in sorted(p for p in folder.iterdir() if p.is_dir()):
        for p in sorted(sub.iterdir()):
            if p.is_dir() and p.name.lower() == 'romfs':
                return p
    return None


def decompressed_size(game, path, raw):
    try:
        return len(game.decompress(raw)[0]) if path.endswith('.zs') else len(raw)
    except Exception:
        return len(raw)


def adapt(home, src, out, zip_path):
    game = Game(home)
    tmp = None
    if src.is_file():
        tmp = Path(tempfile.mkdtemp(prefix='totk-adapt-'))
        with zipfile.ZipFile(src) as z:
            for info in z.infolist():
                target = (tmp / info.filename).resolve()
                if not str(target).startswith(str(tmp.resolve())) or info.is_dir():
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                with z.open(info) as f, open(target, 'wb') as w:
                    shutil.copyfileobj(f, w)
        src = tmp
    root = romfs_root(src)
    if not root:
        raise SystemExit('o mod não tem uma pasta romfs')
    files = sorted(p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file())
    if out.exists():
        shutil.rmtree(out)
    dest = out / 'romfs'
    dest.mkdir(parents=True)
    report = {'copied': 0, 'added': 0, 'text': {}, 'left_out': [], 'warnings': [], 'size_table': None}
    mod_table = None
    table_src = None
    written = {}   # output path -> its bytes' decompressed size (for the size table)
    for n, path in enumerate(files):
        raw = (root / path).read_bytes()
        if n % 100 == 0:
            emit(type='progress', done=n, total=len(files))
        if not path.endswith('.zs') and path + '.zs' in files:
            continue   # an uncompressed copy of a .zs file the mod also has
        if path in game.index:
            (dest / path).parent.mkdir(parents=True, exist_ok=True)
            (dest / path).write_bytes(raw)
            written[path] = decompressed_size(game, path, raw)
            report['copied'] += 1
            continue
        other = game.counterpart(path)
        if other is None:
            if VERSIONED.search(path):
                report['left_out'].append({'path': path, 'why': 'sem equivalente na versão instalada'})
                continue
            (dest / path).parent.mkdir(parents=True, exist_ok=True)
            (dest / path).write_bytes(raw)
            written[path] = decompressed_size(game, path, raw)
            report['added'] += 1
            continue
        if 'ResourceSizeTable' in path:
            mod_table, table_src = restbl_read(game.decompress(raw)[0]), other
            continue
        if path.startswith('Mals/') and path.endswith('.sarc.zs'):
            base_raw = game.read(other)
            base, d = game.decompress(base_raw)
            mine = game.decompress(raw)[0]
            bm, layout = sarc_read(base)
            mm, _ = sarc_read(mine)
            files_changed = replaced = kept = 0
            for name in bm:
                if name in mm and name.endswith('.msbt') and bm[name] != mm[name]:
                    new, r, k = msbt_merge(bm[name], mm[name])
                    if r:
                        bm[name] = new
                        files_changed += 1
                    replaced += r
                    kept += k
                elif name.endswith('.msbt') and name not in mm:
                    kept += len(msbt_labels(next(b for m, b in msbt_sections(bm[name]) if m == b'LBL1')))
            data = sarc_write(bm, layout)
            (dest / other).parent.mkdir(parents=True, exist_ok=True)
            (dest / other).write_bytes(game.compress(data, d))
            written[other] = len(data)
            report['text'][other] = {'from': path, 'files': files_changed, 'messages': replaced, 'untranslated': kept}
            continue
        report['left_out'].append({'path': path, 'why': 'arquivo de interface de outra versão' if 'LayoutArchive' in path
                                   else 'formato que a ferramenta ainda não adapta', 'installed': other})
    emit(type='progress', done=len(files), total=len(files))
    # the size table: the installed one, with room for every file of the adapted mod
    table_path = table_src or next((p for p in game.index if p.startswith('System/Resource/ResourceSizeTable')), None)
    if table_path:
        table_raw = game.read(table_path)
        tdata, td = game.decompress(table_raw)
        table = restbl_read(tdata)
        grown = 0
        for path, size in written.items():
            key = path[:-3] if path.endswith('.zs') else path
            have = restbl_get(table, key)
            want = have or 0
            if mod_table:
                theirs = restbl_get(mod_table, key)   # (same-named files: the mod's own value)
                if theirs:
                    want = max(want, theirs)
            if have is not None and path in game.index:
                orig = decompressed_size(game, path, game.read(path))
                want = max(want, have + max(0, size - orig))
            if have is None and not want:
                continue   # the game keeps no entry for it: none is needed
            if want != have:
                restbl_set(table, key, want)
                grown += 1
        (dest / table_path).parent.mkdir(parents=True, exist_ok=True)
        (dest / table_path).write_bytes(game.compress(restbl_write(table), td))
        report['size_table'] = {'path': table_path, 'entries_changed': grown}
    else:
        report['warnings'].append('a versão instalada não tem tabela de tamanhos')
    (out / 'adapted.json').write_text(json.dumps({'source': src.name if not tmp else None, 'installed_version':
                                                  installed_version(home), **report}, ensure_ascii=False, indent=1),
                                      encoding='utf-8')
    if zip_path:
        zip_path.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_STORED) as z:   # .zs files are compressed already
            for p in sorted(out.rglob('*')):
                if p.is_file():
                    z.write(p, (Path(out.name) / p.relative_to(out)).as_posix())
    if tmp:
        shutil.rmtree(tmp, ignore_errors=True)
    emit(type='result', out=str(out), zip=str(zip_path) if zip_path else None, **report)


def installed_version(home):
    try:
        return json.loads((home / 'totk-install.json').read_text(encoding='utf-8')).get('version')
    except (OSError, ValueError):
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--home', type=Path, required=True)
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--zip', type=Path)
    a = ap.parse_args()
    try:
        adapt(a.home, a.src, a.out, a.zip)
    except SystemExit as e:
        emit(type='error', message=str(e))
        sys.exit(1)
    except Exception as e:
        emit(type='error', message=f'{type(e).__name__}: {e}')
        sys.exit(1)


main()
