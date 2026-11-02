"""The multiplayer server: a room where friends' games meet. Run by one of the players (the setup makes a shortcut,
*Servidor Multijogador*) or on any machine with Python 3.10 or later:

    python mp_server.py [--port 5577] [--password SECRET] [--name "Sala do Arnaldo"]

What it does (runtime/net_mp.c is the game's side; docs/multiplayer.md the protocol):
- keeps the list of players in the room with the progress each game reports after its saves (shrines, lightroots,
  towers, Korok seeds, hearts, achievements);
- relays chat messages and achievement unlocks to everyone;
- keeps a scoreboard and the room's group challenges: six tracks of four tiers the players reach together (shrines
  done by the whole room, say), each tier announced to everyone when it is reached;
- relays voice frames to everyone but their sender;
- relays positions, markers and what each player wears (LOOK), and hands a newcomer the last of each;
- holds four players at most, as in Four Swords: each gets one of four slots (SLOTS), which picks their colour in the
  games (green, red, blue, purple), and keeps it while the slot is free when they come back.

The protocol is UTF-8 text over TCP, one message per line, fields separated by tabs (a tab or a line break inside a
field is sent as a space). Nothing is encrypted: it is for friends, not for strangers on the internet.

Standard library only.
"""
import argparse
import asyncio
import collections
import json
import os
import socket
import time
from pathlib import Path

PROTOCOL = 1
STATS = ('shrines', 'lightroots', 'towers', 'koroks', 'hearts', 'achievements')
MAX_LINE = 4096
MAX_PLAYERS = 4      # named players: the four slots
MAX_CONNECTIONS = 8  # connections, those still saying HELLO included
# Group challenges: one track per figure, four tiers each (bronze, silver, gold, legendary) over the room's sum of
# the best each player reported. Every track advances at once; the game names the tiers in its own language.
TRACKS = [
    ('shrines', (20, 60, 152, 300), 'santuários'),
    ('lightroots', (15, 45, 120, 240), 'raízes de luz'),
    ('towers', (5, 10, 15, 30), 'torres'),
    ('koroks', (100, 300, 800, 1600), 'sementes de Korok'),
    ('hearts', (20, 40, 80, 120), 'corações'),
    ('achievements', (10, 30, 60, 120), 'conquistas'),
]
TIERS = ('Bronze', 'Prata', 'Ouro', 'Lendário')
# The room's log (EVENT lines): what a player's figures say they did, in Portuguese for games that name only the text;
# a newer game words each kind it knows in its own language from the player, the step and the total.
PROGRESS = {
    'shrines': ('{p} completou um santuário ({t}).', '{p} completou {n} santuários ({t}).'),
    'lightroots': ('{p} ativou uma raiz de luz ({t}).', '{p} ativou {n} raízes de luz ({t}).'),
    'towers': ('{p} ativou uma torre ({t}).', '{p} ativou {n} torres ({t}).'),
    'koroks': ('{p} achou uma semente de Korok ({t}).', '{p} achou {n} sementes de Korok ({t}).'),
    'hearts': ('{p} ganhou um recipiente de coração ({t}).', '{p} ganhou {n} recipientes de coração ({t}).'),
}
# what a player's Link does that their POS lines' animation shows: (prefixes, kind, text, seconds before it counts again)
ANIM_EVENTS = [
    (('Nml_Die', 'Swim_Die'), 'death', '{p} morreu.', 10),
    (('WarpHeightIn',), 'warp', '{p} viajou para outro lugar.', 10),
    (('Nml_Get',), 'chest', '{p} abriu um baú.', 3),   # Nml_Get and Nml_Get_Air: Link holds up what a chest gave
]
# the powers, by the actor whose "IsGet.<actor>" flag the game sets when Link gets one (ACT item lines)
POWERS = {'Obj_UltraHand': 'Ultramão', 'Obj_ReverseRecorder': 'Retrocesso', 'Obj_AutoBuilder': 'Autoconstrução',
          'Obj_Camera': 'Câmera'}
LOG_KEEP = 60   # events a newcomer gets


def clean(s, limit=500):
    return str(s).replace('\t', ' ').replace('\r', ' ').replace('\n', ' ')[:limit]


def line(*fields, limit=500):
    """A protocol line; each field at most limit characters (a POS line's body pose is longer than chat may be)."""
    return ('\t'.join(clean(f, limit) for f in fields) + '\n').encode('utf-8')


class Player:
    def __init__(self, writer):
        self.writer = writer
        self.name = ''
        self.stats = {k: 0 for k in STATS}
        self.place = ''
        self.joined = time.time()
        self.last = time.time()
        self.pos = None   # the last POS line's fields (x, y, z)
        self.look = None  # the last LOOK line's fields (head, upper, lower armor names)
        self.slot = -1    # 0..3 once named (Room.free_slot)
        self.stats_known = False   # a STATUS came: later ones that raise a figure are events
        self.anim = ''             # the animation of the last POS line, without its frame
        self.anim_at = {}          # event kind: when its animation last counted
        self.acts = []             # when the last ACT lines came (a few a second at most)
        self.quests = ''           # the last QUESTS line's list: quest:step numbers of the quests under way or done


class Room:
    def __init__(self, name, password, state_path):
        self.name, self.password = name, password
        self.players = []
        self.marks = {}   # player name: ((x, y, z), time), the newest marker of each
        self.log = collections.deque(maxlen=LOG_KEEP)   # the last events, for a newcomer
        self.state_path = state_path
        self.state = {'tiers': {}, 'best': {}, 'slots': {}}
        if state_path and state_path.is_file():
            try:
                self.state.update(json.loads(state_path.read_text(encoding='utf-8')))
            except ValueError:
                pass

    def save(self):
        if self.state_path:
            tmp = self.state_path.with_suffix('.tmp')
            tmp.write_text(json.dumps(self.state, ensure_ascii=False, indent=1), encoding='utf-8')
            os.replace(tmp, self.state_path)

    async def send(self, p, data):
        try:
            p.writer.write(data)
            await asyncio.wait_for(p.writer.drain(), 5)
        except (OSError, asyncio.TimeoutError):
            pass

    async def broadcast(self, data, skip=None):
        for p in list(self.players):
            if p is not skip and p.name:
                await self.send(p, data)

    async def event(self, kind, text, player='', n='', total='', skip=None):
        """An EVENT line to everyone (kind, the text, the player, the step, the total, unix time), kept for newcomers."""
        fields = (kind, text, player, str(n), str(total), str(int(time.time())))
        self.log.append(fields)
        await self.broadcast(line('EVENT', *fields), skip=skip)

    async def send_log(self, p):
        """The kept events to a newcomer, marked as past (its game lists them without notices)."""
        for fields in self.log:
            await self.send(p, line('EVENT', *fields, 'old'))

    def roster(self):
        """ROSTER count (name place stats...)..., then SLOTS count (name slot)...: the games colour each player by slot."""
        named = [p for p in self.players if p.name]
        out = ['ROSTER', str(len(named))]
        for p in named:
            out += [p.name, p.place] + [str(p.stats[k]) for k in STATS]
        slots = ['SLOTS', str(len(named))]
        for p in named:
            slots += [p.name, str(p.slot)]
        return line(*out) + line(*slots)

    def free_slot(self, name):
        """The slot for a player joining: the one they had last time when it is free, else the lowest free one; None
        when all four are taken."""
        taken = {p.slot for p in self.players if p.name}
        last = self.state.setdefault('slots', {}).get(name.lower())
        if isinstance(last, int) and 0 <= last < MAX_PLAYERS and last not in taken:
            return last
        return next((k for k in range(MAX_PLAYERS) if k not in taken), None)

    def total(self, stat):
        # the best each player has reported, so leaving the room keeps the group's progress
        return sum(v.get(stat, 0) for v in self.state['best'].values())

    def challenges_line(self):
        """CHALLENGES count, then per track: figure, the room's total, tiers done, the four targets."""
        out = ['CHALLENGES', str(len(TRACKS))]
        for stat, targets, _ in TRACKS:
            out += [stat, str(self.total(stat)), str(self.state['tiers'].get(stat, 0))] + [str(t) for t in targets]
        return line(*out)

    def challenge_line(self):
        """The one challenge a game from before the tracks shows: the unfinished tier closest to done."""
        best = None
        for stat, targets, noun in TRACKS:
            done = self.state['tiers'].get(stat, 0)
            if done < len(targets):
                k = self.total(stat) / targets[done]
                if best is None or k > best[0]:
                    best = (k, stat, targets[done], f'Juntos, {targets[done]} {noun}')
        if best is None:
            return line('CHALLENGE', 'done', '0', '0', 'Todos os desafios do grupo foram concluídos!')
        return line('CHALLENGE', best[1], str(self.total(best[1])), str(best[2]), best[3])

    async def progress_changed(self):
        best = self.state['best']
        for p in self.players:
            if p.name:
                b = best.setdefault(p.name, {})
                for k in STATS:
                    b[k] = max(b.get(k, 0), p.stats[k])
        tiers = self.state['tiers']
        for stat, targets, noun in TRACKS:
            while tiers.get(stat, 0) < len(targets) and self.total(stat) >= targets[tiers.get(stat, 0)]:
                k = tiers.get(stat, 0)
                tiers[stat] = k + 1
                await self.event('challenge', f'Desafio do grupo concluído: {TIERS[k]} de {noun} ({targets[k]})!')
        self.save()
        await self.broadcast(self.roster())
        await self.broadcast(self.challenges_line())
        await self.broadcast(self.challenge_line())


async def handle(room, reader, writer):
    p = Player(writer)
    peer = writer.get_extra_info('peername')
    sock = writer.get_extra_info('socket')
    if sock is not None:   # each line out at once: positions and voice frames are small and come every 20-50 ms
        try:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass
    if len(room.players) >= MAX_CONNECTIONS:
        writer.write(line('ERROR', 'full', 'A sala está cheia (no máximo 4 jogadores).'))
        writer.close()
        return
    room.players.append(p)
    try:
        while True:
            raw = await asyncio.wait_for(reader.readline(), 120)   # the game pings every 30 s
            if not raw:
                break
            if len(raw) > MAX_LINE:
                break
            f = raw.decode('utf-8', 'replace').rstrip('\r\n').split('\t')
            kind = f[0]
            p.last = time.time()
            if kind == 'HELLO' and len(f) >= 4 and not p.name:
                version, name, password = f[1], clean(f[2]).strip()[:24], f[3]
                if version != str(PROTOCOL):
                    await room.send(p, line('ERROR', 'version', 'Versão do jogo diferente da sala: atualize o jogo.'))
                    break
                if room.password and password != room.password:
                    await room.send(p, line('ERROR', 'password', 'Senha da sala errada.'))
                    break
                if not name or any(q.name.lower() == name.lower() for q in room.players):
                    await room.send(p, line('ERROR', 'name', 'Esse nome já está na sala ou está vazio.'))
                    break
                slot = room.free_slot(name)
                if slot is None:
                    await room.send(p, line('ERROR', 'full', 'A sala está cheia: no máximo 4 jogadores, como em Four Swords.'))
                    break
                p.name, p.slot = name, slot
                room.state['slots'][name.lower()] = slot
                await room.send(p, line('WELCOME', str(PROTOCOL), room.name))
                await room.send_log(p)
                await room.event('join', f'{name} entrou na sala.', name, skip=p)
                for q in room.players:   # where everyone stands and their markers, for the newcomer
                    if q is not p and q.name and q.pos:
                        await room.send(p, line('POS', q.name, *q.pos))
                for q in room.players:   # and what everyone wears, for their figures
                    if q is not p and q.name and q.look:
                        await room.send(p, line('LOOK', q.name, *q.look))
                for q in room.players:   # and where everyone stands in the quests
                    if q is not p and q.name and q.quests:
                        await room.send(p, line('QUESTS', q.name, q.quests, limit=3000))
                for who, (mark, at) in room.marks.items():
                    if time.time() - at < 15 * 60:
                        await room.send(p, line('MARK', who, *mark))
                await room.progress_changed()
                print(f'{time.strftime("%H:%M:%S")} {name} entrou ({peer[0]}) na vaga {slot + 1}', flush=True)
            elif not p.name:
                break
            elif kind == 'STATUS' and len(f) >= 2 + len(STATS):
                p.place = clean(f[1])[:60]
                before = dict(p.stats)
                for k, v in zip(STATS, f[2:2 + len(STATS)]):
                    try:
                        p.stats[k] = max(0, min(100000, int(float(v))))
                    except ValueError:
                        pass
                if p.stats_known:   # the first STATUS is where they stand, not something they did
                    for k, (one, many) in PROGRESS.items():
                        n = p.stats[k] - before[k]
                        if 0 < n <= 50:
                            await room.event(k, (one if n == 1 else many).format(p=p.name, n=n, t=p.stats[k]), p.name,
                                             n, p.stats[k])
                p.stats_known = True
                await room.progress_changed()
            elif kind == 'POS' and len(f) >= 4 and p.name:   # the player's position, to everyone else
                # x, y, z in metres, then (newer games) the way they face in radians, their clock in ms and the name
                # of the animation their Link plays; the newcomer gets only the place, since the clock means nothing
                # to them yet
                # then their whole rotation (a quaternion "x,y,z,w") and the voice clips their Link started (PV000_02,...)
                try:
                    p.pos = tuple(f'{float(v):.2f}' for v in f[1:4])
                    anim = f[6][:63] if len(f) > 6 and f[6].replace('_', '').replace(':', '').replace('.', '').isalnum() \
                        and f[6].isascii() else ''   # Name or Name:frame
                    sfx = f[8][:70] if len(f) > 8 and f[8].replace('_', '').replace(',', '').isalnum() and f[8].isascii() else ''
                    quat = ','.join(f'{float(v):.4f}' for v in f[7].split(',')) if len(f) > 7 and f[7].count(',') == 3 else ''
                    # their Link's real body pose (runtime/link_figure.c link_body_encode: base64url, a fixed length)
                    body = f[9] if len(f) > 9 and len(f[9]) <= 1024 and f[9].isascii() and \
                        f[9].replace('-', '').replace('_', '').isalnum() else ''
                    extra = [f'{float(f[4]):.3f}' if len(f) > 4 and f[4] else '',
                             str(int(f[5])) if len(f) > 5 and f[5] else '', anim,
                             quat, sfx, body]
                    while extra and not extra[-1]:   # an older game's line stays as short as it came
                        extra.pop()
                except ValueError:
                    continue
                base = anim.split(':')[0]
                if base != p.anim:   # an animation began: one the log tells (a death, a fast travel)
                    for prefixes, ev, text, wait in ANIM_EVENTS:
                        if base.startswith(prefixes) and not p.anim.startswith(prefixes) and \
                                time.time() - p.anim_at.get(ev, 0) >= wait:
                            p.anim_at[ev] = time.time()
                            await room.event(ev, text.format(p=p.name), p.name)
                    p.anim = base
                await room.broadcast(line('POS', p.name, *p.pos, *extra, limit=1024) if any(extra) else
                                     line('POS', p.name, *p.pos), skip=p)
            elif kind == 'QUESTS' and len(f) >= 2 and p.name:   # where a player stands in the quests, to everyone else
                # quest:step pairs of numbers (the game's quest list, tools/item_names.py --quests), comma-separated
                if len(f[1]) <= 3000 and all(c in '0123456789:,' for c in f[1]):
                    p.quests = f[1]
                    await room.broadcast(line('QUESTS', p.name, p.quests, limit=3000), skip=p)
            elif kind == 'LOOK' and len(f) >= 4 and p.name:   # the armor a player wears, to everyone else
                p.look = tuple(v if v.startswith('Armor_') and v.replace('_', '').isalnum() else '' for v in
                               (clean(x)[:47] for x in f[1:4]))
                await room.broadcast(line('LOOK', p.name, *p.look), skip=p)
            elif kind == 'MARK' and len(f) >= 4 and p.name:   # a marker, to everyone (its player sees it too)
                try:
                    mark = tuple(f'{float(v):.1f}' for v in f[1:4])
                except ValueError:
                    continue
                room.marks[p.name] = (mark, time.time())
                await room.broadcast(line('MARK', p.name, *mark))
            elif kind == 'VOICE' and len(f) >= 2 and p.name:   # a 20 ms Opus frame, base64: to everyone else
                b64 = ''.join(c for c in f[1][:600] if c.isascii() and (c.isalnum() or c in '+/='))   # line() would cut it at 500
                await room.broadcast(f'VOICE\t{p.name}\t{b64}\n'.encode('utf-8'), skip=p)
            elif kind == 'CHAT' and len(f) >= 2 and f[1].strip():
                await room.broadcast(line('CHAT', p.name, clean(f[1])[:300]))
            elif kind == 'ACH' and len(f) >= 4:   # ACH id name rarity, relayed with the player's name in front
                await room.broadcast(line('ACH', p.name, clean(f[1])[:40], clean(f[2])[:80], clean(f[3])[:16]), skip=p)
            elif kind == 'ACT' and len(f) >= 3 and p.name:   # something the player's game saw them do
                # item <actor>: an item got for the first time (its "IsGet.<actor>" flag); a few a second at most
                now = time.time()
                p.acts = [t for t in p.acts if now - t < 5]
                if f[1] == 'item' and len(f[2]) <= 60 and f[2].isascii() and f[2].replace('_', '').isalnum() and \
                        len(p.acts) < 10:
                    p.acts.append(now)
                    text = f'{p.name} ganhou o poder {POWERS[f[2]]}.' if f[2] in POWERS else \
                        f'{p.name} pegou {f[2]} pela primeira vez.'
                    await room.event('item', text, p.name, f[2])
                # quest <quest> <step> <the step it left>: a quest's step changed (its Step_<quest> enum): from
                # NotReady or Ready (offered) it began, at Complete it is done
                elif f[1] == 'quest' and len(f) >= 5 and len(p.acts) < 10 and all(
                        len(x) <= 64 and x.isascii() and x.replace('_', '').isalnum() for x in f[2:5]):
                    p.acts.append(now)
                    text = (f'{p.name} concluiu a missão {f[2]}.' if f[3] == 'Complete' else
                            f'{p.name} começou a missão {f[2]}.' if f[4] in ('NotReady', 'Ready') else
                            f'{p.name} avançou na missão {f[2]} ({f[3]}).')
                    await room.event('quest', text, p.name, f'{f[2]}:{f[3]}:{f[4]}')
            elif kind == 'PING':
                await room.send(p, line('PONG'))
    except (asyncio.TimeoutError, ConnectionError, OSError):
        pass
    finally:
        room.players.remove(p)
        writer.close()
        if p.name:
            print(f'{time.strftime("%H:%M:%S")} {p.name} saiu', flush=True)
            await room.event('leave', f'{p.name} saiu da sala.', p.name)
            await room.broadcast(room.roster())


def local_addresses():
    out = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            out.add(info[4][0])
    except OSError:
        pass
    return sorted(a for a in out if not a.startswith('127.'))


async def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--port', type=int, default=5577)
    ap.add_argument('--password', default='')
    ap.add_argument('--name', default='Sala de Hyrule')
    ap.add_argument('--state', type=Path,
                    help='where the room keeps its challenge progress (default %%APPDATA%%/TotK Recomp/mp_room.json)')
    args = ap.parse_args()
    state = args.state or Path(os.environ.get('APPDATA', Path.home())) / 'TotK Recomp' / 'mp_room.json'
    state.parent.mkdir(parents=True, exist_ok=True)
    room = Room(args.name, args.password, state)
    server = await asyncio.start_server(lambda r, w: handle(room, r, w), host='0.0.0.0', port=args.port)
    print(f'Servidor Multijogador "{args.name}" na porta {args.port}' + (' (com senha)' if args.password else ''))
    for a in local_addresses():
        print(f'  na sua rede (ou na VPN, como Radmin/Tailscale/ZeroTier): {a}:{args.port}')
    print('Deixe esta janela aberta enquanto jogam. Ctrl+C fecha a sala.', flush=True)
    async with server:
        await server.serve_forever()


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
