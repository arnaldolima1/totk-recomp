"""The mods folder served as RomFS overlays (runtime/romfs.c, runtime/mods.c): load order, added files, case, Unicode.

    .venv\\Scripts\\python tests\\mods_romfs_check.py --exe build\\v121-<tag>\\totk.exe --version v121

Builds a mods folder in a temp directory with files of its own bytes at real RomFS paths (from
build/extracted/<version>/romfs/index.json), runs `totk.exe --probe-fs` on it and reads the `mods:` lines of the log.
--probe-fs compares the bytes it reads through the RomFS with every overlay file, so a wrong winner fails it too.
Needs the extracted RomFS of the version.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UNI = 'Ж мод'


def pick_paths(version):
    index = json.loads((ROOT / 'build/extracted' / version / 'romfs/index.json').read_text())
    paths = [f['path'] for f in index['files'] if 1024 <= f['size'] <= 65536]
    have = set(f['path'] for f in index['files'])
    p1 = next(p for p in paths if any(c.isalpha() for c in p.rsplit('/', 1)[-1]))
    p2 = next(p for p in paths if p != p1)
    i = next(k for k, c in enumerate(p1) if c.isalpha() and k > p1.rfind('/'))
    p1case = p1[:i] + p1[i].swapcase() + p1[i + 1:]
    assert p1case not in have
    return p1, p2, p1case


def put(root, mod, rel, data, part='romfs'):
    f = Path(root) / mod / part / rel
    f.parent.mkdir(parents=True, exist_ok=True)
    f.write_bytes(data * 64)


def run(exe, mods, order, disabled=''):
    env = dict(os.environ, TOTK_MODS_DIR=str(mods), TOTK_MODS_ORDER=order, TOTK_MODS_DISABLED=disabled)
    r = subprocess.run([str(exe), '--probe-fs'], env=env, capture_output=True, timeout=600)
    log = (r.stdout + r.stderr).decode('utf-8', 'replace')
    return r.returncode, log


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--exe', type=Path, required=True)
    ap.add_argument('--version', choices=('v121', 'v143'), required=True)
    args = ap.parse_args()
    p1, p2, p1case = pick_paths(args.version)
    fails = 0

    def check(ok, what, log=None):
        nonlocal fails
        print(('PASS ' if ok else 'FAIL ') + what)
        if not ok:
            fails += 1
            if log:
                print('\n'.join(l for l in log.splitlines() if 'mods' in l or 'overlay' in l.lower())[-3000:])

    with tempfile.TemporaryDirectory() as tmp:
        mods = Path(tmp) / 'mods'
        put(mods, 'Top', p1, b'mod-A')
        put(mods, 'Low', p1, b'mod-B')
        put(mods, 'Low', p2, b'mod-C')
        put(mods, 'Top', 'New/added.bin', b'mod-D')
        put(mods, 'Low', 'New/added.bin', b'mod-E')
        put(mods, 'Case', p1case, b'mod-F')
        put(mods, UNI, p2, b'mod-G')
        # a file of another game version's (no RomFS has a Product.999 table): counted, never served
        put(mods, 'Low', 'System/Resource/ResourceSizeTable.Product.999.rsizetable.zs', b'mod-H')
        put(mods, 'Lite', 'Lite/added.bin', b'mod-I', part='romfslite')   # TKMM's romfslite folder

        code, log = run(args.exe, mods, f'Top|Low|{UNI}|Case')
        check(code == 0, 'probe-fs passes with five mods (served bytes equal the winners\' files)', log)
        for line in ('mods: Top: 2 files served (1 replaced, 1 added), 0 overridden',
                     'mods: Low: 1 files served (1 replaced, 0 added), 2 overridden by Top, 1 for another game version',
                     'mods: Case: 0 files served (0 replaced, 0 added), 1 overridden by Top',
                     f'mods: {UNI}: 0 files served (0 replaced, 0 added), 1 overridden by Low',
                     'mods: Lite: 1 files served (0 replaced, 1 added), 0 overridden'):
            check(line in log, line, log)
        check(p1case not in log, 'the case-flipped path is served as the RomFS path, not added', log)

        code, log = run(args.exe, mods, f'Top|Low|{UNI}|Case', disabled='Top')
        check(code == 0, 'probe-fs passes with Top switched off', log)
        check('mods: Low: 3 files served (2 replaced, 1 added), 0 overridden' in log, 'Low serves P1 and the added file', log)
        check('mods: Top: ' not in log.replace('mods: Top: off', ''), 'a switched-off mod serves nothing', log)
    print('mods_romfs_check:', 'FAIL' if fails else 'ok')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
