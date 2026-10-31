"""A compiler cache for the generated C: one object per (compiler, flags, source, included headers), kept across builds.

The installer lifts the game into a fresh temp folder on every update and used to compile all of it again (about 12
minutes on 16 threads), although a lift of the same game version with the same lifter writes byte-identical chunks.
tools/build_game.py --object-cache DIR puts this script in front of clang on the cc_gen rule:

    python objcache.py --dir DIR --id ID -- clang <flags> -I a -I b -MD -MF x.o.d -c x.c -o x.o

The lookup works like ccache's direct mode. The key is SHA-256 of ID (build_game.py passes the compiler's --version text
and this file's format number), the command line with the include directories, depfile, source and output paths taken
out, and the source's bytes. An entry (DIR/entries/<key>.json) lists every header the last compile of that key read,
each as (root, path below it, SHA-256): root is the index of the -I directory it was found under, or 'tool' for the
compiler's own headers (the compiler version in ID pins those). A hit needs every listed header to still hash the same
below the current -I directories; it copies the object (DIR/objects/<SHA-256>.o, checked against its name) to the
output and writes a depfile naming the current paths, so Ninja sees what a compile would have told it. A miss compiles
and records. Paths therefore may move between builds (another temp folder, another release folder) without a miss.

prune(DIR, since) deletes the entries no compile used since a time and the objects no entry names, so the cache holds
one build's objects (about 310 MiB for 1.4.3). build_game.py calls it after a successful link.

Standard library only.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

FORMAT = 'objcache-1'


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def parse_command(cmd):
    """(normalized args, include dirs, source, object, depfile) of a clang compile command."""
    norm, incs, src, obj, dep = [], [], None, None, None
    i = 1
    while i < len(cmd):
        a = cmd[i]
        nxt = cmd[i + 1] if i + 1 < len(cmd) else None
        if a == '-I':
            norm.append(f'-I#{len(incs)}'); incs.append(Path(nxt).resolve()); i += 2; continue
        if a.startswith('-I') and len(a) > 2:
            norm.append(f'-I#{len(incs)}'); incs.append(Path(a[2:]).resolve()); i += 1; continue
        if a == '-MF':
            dep = Path(nxt); i += 2; continue
        if a == '-o':
            obj = Path(nxt); i += 2; continue
        if a == '-c':
            src = Path(nxt); norm.append('-c'); i += 2; continue
        norm.append(a)
        i += 1
    return norm, incs, src, obj, dep


def read_depfile(path):
    """The prerequisites of a Make-style depfile clang wrote (backslash-escaped spaces, line continuations)."""
    text = Path(path).read_text(encoding='utf-8', errors='surrogateescape').replace('\\\r\n', ' ').replace('\\\n', ' ')
    # the target ends at the first ': ' (a drive letter's colon is followed by a slash)
    text = text[text.index(': ') + 2:]
    words = re.findall(r'(?:\\.|[^\s\\])+', text)
    return [Path(re.sub(r'\\(.)', r'\1', w)) for w in words]


def depfile_text(obj, prereqs):
    def esc(p):
        return p.as_posix().replace(' ', '\\ ').replace('#', '\\#').replace('$', '$$')
    return esc(obj) + ': ' + ' \\\n  '.join(esc(p) for p in prereqs) + '\n'


def classify(dep, src, incs, tool_root):
    """('I<n>', rel) for a header below include dir n, ('tool', rel) below the compiler, ('abs', path) otherwise;
    None for the source itself."""
    p = dep.resolve()
    if p == src.resolve():
        return None
    for n, d in enumerate(incs):
        if p.is_relative_to(d):
            return (f'I{n}', p.relative_to(d).as_posix())
    if tool_root and p.is_relative_to(tool_root):
        return ('tool', p.relative_to(tool_root).as_posix())
    return ('abs', p.as_posix())


def locate(root, rel, incs, tool_root):
    if root.startswith('I'):
        n = int(root[1:])
        return incs[n] / rel if n < len(incs) else None
    if root == 'tool':
        return tool_root / rel if tool_root else None
    return Path(rel)


def lookup(entry_path, incs, tool_root):
    """The object path and current header paths of a valid entry, else None."""
    try:
        entry = json.loads(entry_path.read_text())
    except (OSError, ValueError):
        return None
    paths = []
    for root, rel, digest in entry['deps']:
        p = locate(root, rel, incs, tool_root)
        if p is None or not p.is_file():
            return None
        if root != 'tool' and sha256_file(p) != digest:
            return None
        paths.append(p)
    return entry['obj'], paths


def place(src_file, dst):
    """Copy src_file to dst through a temporary name, so a reader never sees half a file."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst.with_name(f'{dst.name}.{os.getpid()}.tmp')
    shutil.copyfile(src_file, tmp)
    os.replace(tmp, dst)


def compile_cached(cache, ident, cmd):
    norm, incs, src, obj, dep = parse_command(cmd)
    if not (src and obj and dep):
        return subprocess.call(cmd)
    clang = shutil.which(cmd[0]) or cmd[0]
    tool_root = Path(clang).resolve().parent.parent if Path(clang).is_file() else None
    key = hashlib.sha256('\0'.join([FORMAT, ident, *norm, sha256_file(src)]).encode()).hexdigest()
    entry_path = cache / 'entries' / f'{key}.json'
    found = lookup(entry_path, incs, tool_root) if entry_path.is_file() else None
    if found:
        digest, paths = found
        stored = cache / 'objects' / f'{digest}.o'
        if stored.is_file() and sha256_file(stored) == digest:
            place(stored, obj)
            dep.write_text(depfile_text(obj, [src, *paths]), encoding='utf-8')
            os.utime(entry_path)   # used by this build: prune keeps it
            return 0
        stored.unlink(missing_ok=True)
    code = subprocess.call(cmd)
    if code:
        return code
    try:
        deps = []
        for d in read_depfile(dep):
            c = classify(d, src, incs, tool_root)
            if c is not None:
                deps.append([c[0], c[1], '' if c[0] == 'tool' else sha256_file(d)])
        digest = sha256_file(obj)
        stored = cache / 'objects' / f'{digest}.o'
        if not stored.is_file():
            place(obj, stored)
        entry_path.parent.mkdir(parents=True, exist_ok=True)
        tmp = entry_path.with_name(f'{entry_path.name}.{os.getpid()}.tmp')
        tmp.write_text(json.dumps({'obj': digest, 'deps': deps}))
        os.replace(tmp, entry_path)
    except OSError as e:   # the object is built; a cache that cannot record it only costs the next build time
        print(f'objcache: not recorded: {e}', file=sys.stderr)
    return 0


def prune(cache, since):
    """Delete entries not used since `since` (a time.time() value) and objects no remaining entry names. Returns
    (entries kept, entries deleted, objects deleted)."""
    cache = Path(cache)
    kept, dropped, names = 0, 0, set()
    for e in (cache / 'entries').glob('*.json'):
        try:
            if e.stat().st_mtime < since:
                e.unlink(); dropped += 1; continue
            names.add(json.loads(e.read_text())['obj'] + '.o')
            kept += 1
        except (OSError, ValueError):
            e.unlink(missing_ok=True); dropped += 1
    gone = 0
    for o in (cache / 'objects').glob('*'):
        if o.name not in names:
            o.unlink(missing_ok=True); gone += 1
    for t in cache.glob('*/*.tmp'):
        t.unlink(missing_ok=True)
    return kept, dropped, gone


def main():
    argv = sys.argv[1:]
    if '--' not in argv:
        sys.exit('usage: objcache.py --dir DIR --id ID -- <compiler command>')
    split = argv.index('--')
    opts, cmd = argv[:split], argv[split + 1:]
    cache = Path(opts[opts.index('--dir') + 1])
    ident = opts[opts.index('--id') + 1]
    sys.exit(compile_cached(cache, ident, cmd))


if __name__ == '__main__':
    main()
