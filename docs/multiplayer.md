# Multiplayer

Friends play side by side, each in their own world: a room shares everyone's progress, their achievement unlocks and
a chat, and keeps a group challenge the players reach together. No game state crosses between the games; seeing the
others in the world is later work (layers 2 and 3 below).

## Playing

1. One player opens the room with the *Servidor Multijogador* shortcut the setup puts in the install folder and the
   Start menu (`tools/mp_server.py` in a console window). The window lists the addresses friends can join. Friends in
   another house reach it through a VPN that joins the machines (Radmin VPN, Tailscale, ZeroTier) or a forwarded TCP
   port (5577 by default). The same script runs on any server with Python 3.10 or later:
   `python3 mp_server.py --port 5577 --password SECRET --name "My room"`.
2. Every player opens the menu (F1) on the *Multiplayer* tab, sets a name, the server (`address:port`) and the room's
   password, and presses *Connect*. The settings are `mp`, `mp_server`, `mp_name` and `mp_password` in `totk.ini`.
3. The tab lists the players with their shrines, lightroots, towers, Korok seeds, hearts and achievements, the group
   challenge with its progress, and the chat. Chat lines, friends' unlocks and the room's events also show at the
   bottom left of the screen for a few seconds; a friend's unlock shows its icon when this install has that
   achievement.

Progress is what each game's newest save holds, read after every save with the achievements (`runtime/achievements.c`
`ach_stats`, from the `S` lines `tools/achievements.py` compiles), so it changes when the game saves.

## Protocol

UTF-8 text over TCP, one message per line, fields separated by tabs; a tab or line break inside a field is sent as a
space. Nothing is encrypted or authenticated beyond the room's password: rooms are for friends.

| From | Message | Fields |
|---|---|---|
| game | `HELLO` | protocol (`1`), name, password |
| game | `STATUS` | place (empty for now), shrines, lightroots, towers, Korok seeds, hearts, achievements |
| game | `CHAT` | text |
| game | `ACH` | achievement id, name, rarity |
| game | `PING` | (every 30 s; the room drops a game silent for 120 s) |
| room | `WELCOME` | protocol, room name |
| room | `ERROR` | code (`version`, `password`, `name`, `full`), text |
| room | `ROSTER` | count, then per player: name, place and the six figures of `STATUS` |
| room | `CHAT` | player, text |
| room | `ACH` | player, achievement id, name, rarity |
| room | `EVENT` | kind (`join`, `leave`, `challenge`), text |
| room | `CHALLENGE` | figure (or `done`), the room's total, target, text |
| room | `PONG` | |

The room keeps, per player name, the best figures each has reported; a group challenge counts the sum of those, so a
player leaving does not undo the room's progress. The challenges go in order (`tools/mp_server.py` `CHALLENGES`) and the
room's place in them is kept in `%APPDATA%/TotK Recomp/mp_room.json`.

`runtime/net_mp.c` runs the game's side on a thread of its own: it connects, sends `STATUS` after each evaluated save
and `ACH` for each unlock (`ach_take_net_unlock`), relays the menu's chat, pings, and reconnects every 5 s after a lost
connection. A refusal (`ERROR`) stops the retries until the settings change or *Connect* is pressed again.

## Later layers

2. Presence: the player's position read from the game, for a radar towards friends and shared map markers.
3. Friends in the world: an actor per friend, moved by the position and animation the room relays.

Both need the game's live data (position, actors) located in the 1.4.3 code, which the save does not hold.
