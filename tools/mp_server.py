"""The multiplayer server: a room where friends' games meet. Run by one of the players (the setup makes a shortcut,
*Servidor Multijogador*) or on any machine with Python 3.10 or later:

    python mp_server.py [--port 5577] [--password SECRET] [--name "Sala do Arnaldo"]

What it does (runtime/net_mp.c is the game's side; docs/multiplayer.md the protocol):
- keeps the list of players in the room with the progress each game reports after its saves (shrines, lightroots,
  towers, Korok seeds, hearts, achievements);
- relays chat messages and achievement unlocks to everyone;
- keeps a scoreboard and the room's group challenge: a target the players reach together (shrines done by the whole
  room, say), announced to everyone when it is reached.

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
# Group challenges: (stat, target over the room's sum, Portuguese text). The room goes through them in order.
CHALLENGES = [
    ('shrines', 10, 'Juntos, concluam 10 santuários'),
    ('lightroots', 15, 'Juntos, acendam 15 raízes de luz'),
    ('towers', 10, 'Juntos, ativem 10 torres'),
    ('shrines', 50, 'Juntos, concluam 50 santuários'),
    ('koroks', 100, 'Juntos, juntem 100 sementes de Korok'),
    ('lightroots', 60, 'Juntos, acendam 60 raízes de luz'),
    ('achievements', 60, 'Juntos, desbloqueiem 60 conquistas'),
    ('shrines', 152, 'Juntos, concluam 152 santuários'),
]


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


class Room:
    def __init__(self, name, password, state_path):
        self.name, self.password = name, password
        self.players = []
        self.state_path = state_path
        self.state = {'challenge': 0, 'best': {}}
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

    def challenge_line(self):
        i = self.state['challenge']
        if i >= len(CHALLENGES):
            return line('CHALLENGE', 'done', '0', '0', 'Todos os desafios do grupo foram concluídos!')
        stat, target, text = CHALLENGES[i]
        total = sum(p.stats[stat] for p in self.players if p.name)
        # the best each player has reported counts too, so leaving the room keeps the group's progress
        best = self.state['best']
        total = max(total, sum(v.get(stat, 0) for v in best.values()))
        return line('CHALLENGE', stat, str(total), str(target), text)

    async def progress_changed(self):
        best = self.state['best']
        for p in self.players:
            if p.name:
                b = best.setdefault(p.name, {})
                for k in STATS:
                    b[k] = max(b.get(k, 0), p.stats[k])
        while self.state['challenge'] < len(CHALLENGES):
            stat, target, text = CHALLENGES[self.state['challenge']]
            if sum(v.get(stat, 0) for v in best.values()) < target:
                break
            self.state['challenge'] += 1
            await self.broadcast(line('EVENT', 'challenge', f'Desafio do grupo concluído: {text}!'))
        self.save()
        await self.broadcast(self.roster())
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
