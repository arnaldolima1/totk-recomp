"""Builds the sounds the game plays for each friend's Link in a multiplayer room (runtime/friend_sfx.c): footsteps,
jumps and landings on three grounds, sword swings, drawing and sheathing, climbing, swimming, the paraglider and the
roll, and Link's voice, decoded from the player's own RomFS. The result stays in the profile (never in this repository).

    python tools/link_sounds.py <romfs dir> <out.bin>

The game keeps its sound effects in Sound/Resource/<name>.bars.zs (zstd, with the RomFS's dictionaries in
Pack/ZsDic.pack.zs). A BARS file: b'BARS', u32 size, u16 byte order mark, u16 version, u32 count, count CRC32 name
hashes, then per entry u32 offset of its AMTA (metadata, whose name string ends the block) and u32 offset of its BWAV.
A BWAV: b'BWAV', u16 byte order mark, u16 version, u32 hash, u16 flags, u16 channel count, then 0x4c bytes per channel:
u16 codec (1 = Nintendo DSP ADPCM), u16 pan, u32 sample rate, u32 sample count, u32 sample count kept in this file (a
longer sound streams the rest from Sound/Resource/Stream/<name>.bwav, which holds all of it), 16 i16 coefficients,
u32 offset of the samples kept here, u32 offset of the samples in the stream file, then loop data. DSP ADPCM: frames of
8 bytes, 14 samples each; the first byte holds the coefficient pair (high nibble) and the shift (low nibble), then 14
signed nibbles; sample = (nibble << shift << 11 + 1024 + c1 * h1 + c2 * h2) >> 11, clamped to 16 bits.

Output (little-endian): b'LNKS', u32 version 1, u32 clip count; per clip its name (48 bytes), u32 sample rate, u32
sample count and its samples, mono i16.
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import link_figure  # noqa: E402  (RomFS reader, SARC members)
import txtg  # noqa: E402  (zstd)

MATERIALS = ['Grass', 'Soil', 'Stone']   # runtime/friend_sfx.c picks one by the layer the friend is in
# (archive, name pattern): every clip whose name matches goes in; runtime/friend_sfx.c plays them by name without the
# trailing variant number, one of the variants at random
WANTED = [
    ('M_Mt_{m}', r'^Mt_{m}_Pl_(MoveSlow|MoveFast)_Heel[LR]\d+$'),
    ('M_Mt_{m}', r'^Mt_{m}_Pl_ClimbStrong_Hand[LR]\d+$'),
    ('M_Mt_Static', r'^Mt_{m}_Pl_(Jump|LandMiddle|LandWeak)_\d+$'),
    ('Weapon_Direct', r'^NSword_Whoosh_Player_M_Sharp_Metal_\d+$'),
    ('Weapon_Direct', r'^NSword_WhooshFinish_Player_M_Sharp_Metal_\d+$'),
    ('Weapon_Direct_Sword', r'^Weapon_Sword_Metal_(Equip|UnEquip)\d+$'),
    ('Player', r'^Pl_FrontRoll\d+$'),
    ('Player', r'^Pl_Parashawl_(Equip|UnEquip)\d+$'),
    ('Player', r'^Water_Pl_SwimHand_Small(Slow|Fast)\d+$'),
    ('Player', r'^Water_Pl_SwimKick_Hand\d+$'),
    ('Player', r'^PL_Water_SwimDash_\d+$'),
    ('Player_Voice', r'^PV\d{3}_\d{2}$'),   # Link's voice: the friend's game names the one it plays (net_mp.c)
]
HALF_RATE = ('PV',)   # voices are kept at half their rate: 337 of them, about a second each
MAX_SECONDS = 2.5   # a long clip is cut (a fade over its last 50 ms): these are one-shot sounds


def unzstd(rom, dicts, path):
    frame = rom.read(path)
    if frame is None:
        raise FileNotFoundError(f'{path} is not in the RomFS')
    fhd = frame[4]   # the frame header names its dictionary: a 1-, 2- or 4-byte id after the descriptor (and window)
    size = (0, 1, 2, 4)[fhd & 3]
    at = 5 + (0 if fhd & 0x20 else 1)
    did = int.from_bytes(frame[at:at + size], 'little') if size else 0
    return txtg.unzstd(frame, dicts.get(did))


def bars_entries(d):
    """(name, BWAV offset) of each entry of a BARS file."""
    if d[:4] != b'BARS':
        raise ValueError('not a BARS file')
    n = struct.unpack_from('<I', d, 12)[0]
    table = 16 + 4 * n
    for i in range(n):
        amta, bwav = struct.unpack_from('<II', d, table + 8 * i)
        m = re.search(rb'([A-Za-z][A-Za-z0-9_]{3,})\x00', d[amta + 0x20:amta + 0x200])
        if m:
            yield m.group(1).decode(), bwav


def dsp_decode(data, at, count, coefs):
    out, h1, h2 = [], 0, 0
    while len(out) < count and at + 8 <= len(data):
        head = data[at]
        c1, c2 = coefs[2 * (head >> 4)], coefs[2 * (head >> 4) + 1]
        shift = head & 15
        for k in range(14):
            if len(out) >= count:
                break
            b = data[at + 1 + k // 2]
            nib = (b >> 4) if k % 2 == 0 else (b & 15)
            if nib >= 8:
                nib -= 16
            v = ((nib << shift) * 2048 + 1024 + c1 * h1 + c2 * h2) >> 11
            v = -32768 if v < -32768 else 32767 if v > 32767 else v
            out.append(v)
            h2, h1 = h1, v
        at += 8
    return out


def bwav_clip(d, at, stream):
    """(rate, mono samples) of the BWAV at offset at; its first channel, from the stream file when only a part is
    kept here."""
    if d[at:at + 4] != b'BWAV':
        raise ValueError('not a BWAV')
    ch = at + 0x10
    codec, _pan, rate, total, kept = struct.unpack_from('<HHIII', d, ch)
    if codec != 1:
        raise ValueError(f'codec {codec} not handled')
    coefs = struct.unpack_from('<16h', d, ch + 0x10)
    here, there = struct.unpack_from('<II', d, ch + 0x30)
    count = min(total, int(rate * MAX_SECONDS))
    if kept >= count:
        return rate, dsp_decode(d, at + here, count, coefs)
    s = stream()
    if s is None or s[:4] != b'BWAV':
        return rate, dsp_decode(d, at + here, min(kept, count), coefs)
    sh, st, sr, stotal, _ = struct.unpack_from('<HHIII', s, 0x10)
    scoefs = struct.unpack_from('<16h', s, 0x20)
    return rate, dsp_decode(s, struct.unpack_from('<I', s, 0x40)[0], min(stotal, count), scoefs)


def main(argv):
    if len(argv) != 3:
        sys.exit(__doc__)
    rom = link_figure.RomFS(argv[1])
    dicts = {}
    for blob in link_figure.sarc_members(txtg.unzstd(rom.read('Pack/ZsDic.pack.zs'))).values():
        if blob[:4] == b'\x37\xa4\x30\xec':   # a zstd dictionary: its id follows the magic
            dicts[struct.unpack_from('<I', blob, 4)[0]] = txtg.zstd_dict(blob)
    wanted = {}
    for archive, pattern in WANTED:
        for m in (MATERIALS if '{m}' in archive or '{m}' in pattern else [None]):
            pat = pattern.replace('{m}', m) if m else pattern   # str.format would read the regex's {3}
            wanted.setdefault(archive.replace('{m}', m) if m else archive, []).append(re.compile(pat))
    clips = []
    for archive, patterns in wanted.items():
        d = unzstd(rom, dicts, f'Sound/Resource/{archive}.bars.zs')
        for name, at in bars_entries(d):
            if not any(p.match(name) for p in patterns):
                continue
            rate, pcm = bwav_clip(d, at, lambda: rom.read(f'Sound/Resource/Stream/{name}.bwav'))
            if name.startswith(HALF_RATE) and rate >= 32000:   # each pair of samples averaged
                pcm = [(pcm[k] + pcm[k + 1]) // 2 for k in range(0, len(pcm) - 1, 2)]
                rate //= 2
            fade = min(len(pcm), rate // 20)
            if len(pcm) >= int(rate * MAX_SECONDS):   # cut: fade out the end
                for k in range(fade):
                    pcm[-1 - k] = pcm[-1 - k] * k // fade
            clips.append((name, rate, pcm))
    clips.sort()
    out = bytearray(b'LNKS') + struct.pack('<II', 1, len(clips))
    for name, rate, pcm in clips:
        out += name.encode()[:47].ljust(48, b'\0') + struct.pack('<II', rate, len(pcm))
        out += struct.pack(f'<{len(pcm)}h', *pcm)
    Path(argv[2]).write_bytes(out)
    seconds = sum(len(p) / r for _, r, p in clips)
    print(f'{len(clips)} clips, {seconds:.1f} s of sound, {len(out) >> 10} KiB -> {argv[2]}')


if __name__ == '__main__':
    main(sys.argv)
