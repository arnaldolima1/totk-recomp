"""The multiplayer server: a room where friends' games meet. Run by one of the players (the setup makes a shortcut,
*Servidor Multijogador*) or on any machine with Python 3.10 or later:

    python mp_server.py [--port 5577] [--password SECRET] [--name "Sala do Arnaldo"]

What it does (runtime/net_mp.c is the game's side; docs/multiplayer.md the protocol):
- keeps the list of players in the room with the progress each game reports after its saves (shrines, lightroots,
  towers, Korok seeds, hearts, achievements);
- relays chat messages and achievement unlocks to everyone;
- keeps a scoreboard and the room's group challenges: six tracks of four tiers the players reach together (shrines
  done by the whole room, say), each tier announced to everyone when it is reached;
- relays voice frames to everyone but their sender.

The protocol is UTF-8 text over TCP, one message per line, fields separated by tabs (a tab or a line break inside a
field is sent as a space). Nothing is encrypted: it is for friends, not for strangers on the internet.

Standard library only.
"""
import argparse
import asyncio
import json
import os
import socket
import time
from pathlib import Path

PROTOCOL = 1
STATS = ('shrines', 'lightroots', 'towers', 'koroks', 'hearts', 'achievements')
MAX_LINE = 4096
MAX_PLAYERS = 32
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


def clean(s):
    return str(s).replace('\t', ' ').replace('\r', ' ').replace('\n', ' ')[:500]


def line(*fields):
    return ('\t'.join(clean(f) for f in fields) + '\n').encode('utf-8')


class Player:
    def __init__(self, writer):
        self.writer = writer
        self.name = ''
        self.stats = {k: 0 for k in STATS}
        self.place = ''
        self.joined = time.time()
        self.last = time.time()
        self.pos = None   # the last POS line's fields (x, y, z)


class Room:
    def __init__(self, name, password, state_path):
        self.name, self.password = name, password
        self.players = []
        self.marks = {}   # player name: ((x, y, z), time), the newest marker of each
        self.state_path = state_path
        self.state = {'tiers': {}, 'best': {}}
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

    def roster(self):
        out = ['ROSTER', str(len([p for p in self.players if p.name]))]
        for p in self.players:
            if p.name:
                out += [p.name, p.place] + [str(p.stats[k]) for k in STATS]
        return line(*out)

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
                await self.broadcast(line('EVENT', 'challenge',
                                          f'Desafio do grupo concluído: {TIERS[k]} de {noun} ({targets[k]})!'))
        self.save()
        await self.broadcast(self.roster())
        await self.broadcast(self.challenges_line())
        await self.broadcast(self.challenge_line())


async def handle(room, reader, writer):
    p = Player(writer)
    peer = writer.get_extra_info('peername')
    if len(room.players) >= MAX_PLAYERS:
        writer.write(line('ERROR', 'full', 'A sala está cheia.'))
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
                p.name = name
                await room.send(p, line('WELCOME', str(PROTOCOL), room.name))
                await room.broadcast(line('EVENT', 'join', f'{name} entrou na sala.'), skip=p)
                for q in room.players:   # where everyone stands and their markers, for the newcomer
                    if q is not p and q.name and q.pos:
                        await room.send(p, line('POS', q.name, *q.pos))
                for who, (mark, at) in room.marks.items():
                    if time.time() - at < 15 * 60:
                        await room.send(p, line('MARK', who, *mark))
                await room.progress_changed()
                print(f'{time.strftime("%H:%M:%S")} {name} entrou ({peer[0]})', flush=True)
            elif not p.name:
                break
            elif kind == 'STATUS' and len(f) >= 2 + len(STATS):
                p.place = clean(f[1])[:60]
                for k, v in zip(STATS, f[2:2 + len(STATS)]):
                    try:
                        p.stats[k] = max(0, min(100000, int(float(v))))
                    except ValueError:
                        pass
                await room.progress_changed()
            elif kind == 'POS' and len(f) >= 4 and p.name:   # the player's position, to everyone else
                try:
                    p.pos = tuple(f'{float(v):.1f}' for v in f[1:4])
                except ValueError:
                    continue
                await room.broadcast(line('POS', p.name, *p.pos), skip=p)
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
            elif kind == 'PING':
                await room.send(p, line('PONG'))
    except (asyncio.TimeoutError, ConnectionError, OSError):
        pass
    finally:
        room.players.remove(p)
        writer.close()
        if p.name:
            print(f'{time.strftime("%H:%M:%S")} {p.name} saiu', flush=True)
            await room.broadcast(line('EVENT', 'leave', f'{p.name} saiu da sala.'))
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
