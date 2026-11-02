"""The names the game gives its items, for the multiplayer room's log (runtime/net_mp.c event_text): each actor's
"<actor>_Name" message in the game's own texts (Mals/<language>.Product.<version>.sarc.zs, ActorMsg/*.msbt) in
English, Brazilian Portuguese and Russian.

    python tools/item_names.py <romfs dir> <out.tsv> [--quests <quests.tsv>]

Output: UTF-8 lines "actor<TAB>English<TAB>Portuguese<TAB>Russian", sorted by actor; an empty field where a language has
no name. With --quests also the game's quests (RSDB/Challenge, the quest table): lines "quest<TAB>category<TAB>steps
<TAB>English<TAB>Portuguese<TAB>Russian", the steps comma-separated in order; a quest's progress is the Enum flag
Step_<quest>, whose values are NotReady, those steps and Complete (runtime/net_mp.c quest watch). Next to it,
quest-steps.tsv: "quest<TAB>step<TAB>English<TAB>Portuguese<TAB>Russian", the adventure log's text at each step (and
at Complete). The installer keeps them in profile/ (installer/steps.py, prepare).
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from link_figure import RomFS, sarc_members, unzstd_file, zstd_dicts  # noqa: E402

LANGUAGES = ('USen', 'USpt', 'EUru')


def msbt_texts(data):
    """{label: text} of an MSBT file, its control tags left out."""
    assert data[:8] == b'MsgStdBn', 'not an MSBT file'
    e = '<' if data[8:10] == b'\xff\xfe' else '>'
    count = struct.unpack_from(e + 'H', data, 0x0e)[0]
    at, labels, texts = 0x20, {}, []
    for _ in range(count):
        magic, size = data[at:at + 4], struct.unpack_from(e + 'I', data, at + 4)[0]
        body = at + 16
        if magic == b'LBL1':
            groups = struct.unpack_from(e + 'I', data, body)[0]
            for g in range(groups):
                n, off = struct.unpack_from(e + 'II', data, body + 4 + 8 * g)
                p = body + off
                for _ in range(n):
                    length = data[p]
                    name = data[p + 1:p + 1 + length].decode('utf-8', 'replace')
                    labels[name] = struct.unpack_from(e + 'I', data, p + 1 + length)[0]
                    p += 1 + length + 4
        elif magic == b'TXT2':
            n = struct.unpack_from(e + 'I', data, body)[0]
            offs = [struct.unpack_from(e + 'I', data, body + 4 + 4 * i)[0] for i in range(n)] + [size]
            for i in range(n):
                raw = data[body + offs[i]:body + offs[i + 1]]
                texts.append(utf16_text(raw, e))
        at = (body + size + 15) & ~15
    return {name: texts[i] for name, i in labels.items() if i < len(texts)}


def utf16_text(raw, e):
    """A TXT2 string: UTF-16 up to its terminator, control tags (0x0E group type size data, 0x0F group type) dropped."""
    out, i = [], 0
    while i + 1 < len(raw):
        c = struct.unpack_from(e + 'H', raw, i)[0]
        if c == 0:
            break
        if c == 0x0e:
            size = struct.unpack_from(e + 'H', raw, i + 6)[0]
            i += 8 + size
            continue
        if c == 0x0f:
            i += 6
            continue
        out.append(raw[i:i + 2])
        i += 2
    return b''.join(out).decode('utf-16-le' if e == '<' else 'utf-16-be', 'replace').strip()


def names(romfs, dicts, language):
    """{actor: name} from one language's texts."""
    path = next((p for p in romfs.idx if re.fullmatch(rf'Mals/{language}\.Product\.\d+\.sarc\.zs', p)), None)
    if path is None:
        return {}
    out = {}
    for member, data in sarc_members(unzstd_file(romfs, dicts, path)).items():
        if member.startswith('ActorMsg/') and member.endswith('.msbt'):
            for label, text in msbt_texts(data).items():
                if label.endswith('_Name') and text:
                    out[label[:-5]] = text
    return out


def quest_texts(romfs, dicts, language):
    """{quest: {label: text}} of one language's quest texts (ChallengeMsg/Info_<quest>.msbt): the title (Name), and the
    adventure log's text of each step (labelled by the step's name) and of the quest done (Complete)."""
    path = next((p for p in romfs.idx if re.fullmatch(rf'Mals/{language}\.Product\.\d+\.sarc\.zs', p)), None)
    if path is None:
        return {}
    out = {}
    for member, data in sarc_members(unzstd_file(romfs, dicts, path)).items():
        if member.startswith('ChallengeMsg/Info_') and member.endswith('.msbt'):
            out[member[len('ChallengeMsg/Info_'):-len('.msbt')]] = msbt_texts(data)
    return out


def quests(romfs, dicts):
    """[(quest, category, [step names])] of the game's quest table (RSDB/Challenge.Product.<version>.rstbl.byml.zs)."""
    from achievements import read_byml
    path = max((p for p in romfs.idx if re.fullmatch(r'RSDB/Challenge\.Product\.\d+\.rstbl\.byml\.zs', p)),
               key=lambda p: int(p.split('.')[2]))
    out = []
    for row in read_byml(unzstd_file(romfs, dicts, path)):
        quest = re.search(r'Challenge/(.+?)\.game__', row['__RowId']).group(1)
        out.append((quest, row.get('Category', ''), [s['Name'] for s in row.get('Steps', [])]))
    return sorted(out)


def clean(s):
    return s.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ')


def main(argv):
    quests_out = None
    if '--quests' in argv:   # --quests <out.tsv>: the quest list too
        i = argv.index('--quests')
        quests_out = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    if len(argv) != 3:
        sys.exit(__doc__)
    romfs = RomFS(argv[1])
    dicts = zstd_dicts(romfs)
    by_lang = [names(romfs, dicts, lang) for lang in LANGUAGES]
    actors = sorted(set().union(*by_lang))
    with open(argv[2], 'w', encoding='utf-8', newline='\n') as f:
        for a in actors:
            f.write('\t'.join([a] + [clean(n.get(a, '')) for n in by_lang]) + '\n')
    print(f'{len(actors)} item names ({", ".join(f"{l} {len(n)}" for l, n in zip(LANGUAGES, by_lang))}) -> {argv[2]}')
    if quests_out:
        texts = [quest_texts(romfs, dicts, lang) for lang in LANGUAGES]
        rows = quests(romfs, dicts)
        steps_out = Path(quests_out).with_name('quest-steps.tsv')
        with open(quests_out, 'w', encoding='utf-8', newline='\n') as f, \
                open(steps_out, 'w', encoding='utf-8', newline='\n') as g:
            for quest, category, steps in rows:
                f.write('\t'.join([quest, category, ','.join(steps)] +
                                  [clean(t.get(quest, {}).get('Name', '')) for t in texts]) + '\n')
                for step in steps + ['Complete']:
                    line = [clean(t.get(quest, {}).get(step, '')) for t in texts]
                    if any(line):
                        g.write('\t'.join([quest, step] + line) + '\n')
        print(f'{len(rows)} quests -> {quests_out}, their steps\' texts -> {steps_out}')


if __name__ == '__main__':
    main(sys.argv)
