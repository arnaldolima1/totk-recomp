# Multiplayer

Friends play side by side, each in their own world: a room shares everyone's progress, their achievement unlocks,
a text chat and a voice chat, and keeps a group challenge the players reach together. No game state crosses between the games; seeing the
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
   achievement. A player's own chat lines show in the chat, without a notice.
4. In the game, the chat key (T by default) opens a chat box at the bottom left: Enter sends and goes back to the game,
   Esc closes it, and the game gets no keys meanwhile. Voice chat is on by default: hold the push-to-talk key (V) to
   talk, or set the microphone to open. Who is talking shows at the top left. The settings are on the *Sound* tab:
   `mp_chat_key` (`t`, `y`, `enter`, `off`), `mp_voice` (0/1), `mp_voice_mode` (`push`, `open`), `mp_voice_key` (`v`,
   `b`, `g`, `h`, `capslock`) and `mp_voice_vol` (0 to 200 %). The push-to-talk key also reaches the game, so it should
   be one the controls leave free; headphones keep the game and the friends' voices out of the microphone.

## Voice

`runtime/voice.c`. A thread of its own opens the default microphone through SDL3 while the game is in a room with voice
on, and reads it in 20 ms frames (48 kHz mono). With push to talk the microphone is paused while the key is up and its
buffer is cleared when the key goes down; with the open microphone a frame is sent while its level is over about
-38 dBFS and for 400 ms after. Frames are Opus (VOIP, 24 kbit/s, about 64 bytes) and wait in a queue of 32 that the
connection's thread empties every 10 ms. Each friend's frames are decoded into a one-second ring; SDL's audio thread
(`runtime/audio_mixer.c` `sdl_pull`) mixes a ring once 60 ms are buffered and stops when it runs dry, so a late frame
costs a short gap rather than a growing delay. A friend counts as talking for 400 ms after their last frame.

Changing the room's settings reconnects; other settings, the voice's among them, leave the connection alone.

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
| game | `VOICE` | an Opus frame, base64 (20 ms of 48 kHz mono) |
| game | `POS` | x, y, z: the player's position (each 200 ms while moving, else each 2 s) |
| game | `MARK` | x, y, z: a marker where the player stands |
| game | `PING` | (every 30 s; the room drops a game silent for 120 s) |
| room | `WELCOME` | protocol, room name |
| room | `ERROR` | code (`version`, `password`, `name`, `full`), text |
| room | `ROSTER` | count, then per player: name, place and the six figures of `STATUS` |
| room | `CHAT` | player, text |
| room | `ACH` | player, achievement id, name, rarity |
| room | `VOICE` | player, the frame as received (to everyone but its sender) |
| room | `POS` | player, x, y, z (to everyone else; the last of each to a player who joins) |
| room | `MARK` | player, x, y, z (to everyone, its player too; markers under 15 minutes old to a player who joins) |
| room | `EVENT` | kind (`join`, `leave`, `challenge`), text |
| room | `CHALLENGES` | count, then per track: figure, the room's total, tiers reached, four targets |
| room | `CHALLENGE` | figure (or `done`), the room's total, target, text (the closest tier, for older games) |
| room | `PONG` | |

The room keeps, per player name, the best figures each has reported; a group challenge counts the sum of those, so a
player leaving does not undo the room's progress. There is one track per figure with four tiers (bronze, silver, gold,
legendary; `tools/mp_server.py` `TRACKS`), 24 challenges that advance together; `CHALLENGES` sends every track (figure,
the room's total, tiers reached, the four targets) and `CHALLENGE` the one closest to done for games from before the
tracks. The tiers reached are kept in `%APPDATA%/TotK Recomp/mp_room.json`.

## Multiplayer achievements

The game counts what happens in rooms (`runtime/net_mp.c`): rooms joined, the most players together, chat lines sent,
seconds of voice sent, minutes in a room with friends, friends' unlocks seen, its own unlocks with friends in the room,
and the group tiers reached. The counters live in `<profile>/achievements-mp.tsv`; achievements over them (condition
kind `mp`, `tools/achievements.py` presets `mp_*`) unlock at once, stay outside the platinum and count for 100 %. The
default set has eleven.

`runtime/net_mp.c` runs the game's side on a thread of its own: it connects, sends `STATUS` after each evaluated save
and `ACH` for each unlock (`ach_take_net_unlock`), relays the menu's chat, pings, and reconnects every 5 s after a lost
connection. A refusal (`ERROR`) stops the retries until the settings change or *Connect* is pressed again.

## Presence

`runtime/frame_rate.c` `game_player_pos` reads the player's position from the game: the scene the main module points
to, its resident actors (component 13), the one named `Player`, its position at +0x2b4 (the walk MacNativeGames
documents for each version). x is east, y up, z south, in metres, as the game's map shows them (it prints -z). The
game sends it in `POS` lines and keeps its friends' by name for six seconds.

The radar (`runtime/ui/menu.cpp` `radar_draw`, setting `mp_radar`) is north up, as the map: friends are dots with their
initial, hollow with an arrow when in another layer, and markers are diamonds. The layer comes from the height (`mp_layer`:
sky over 900 m, depths under -200 m). The mark key (`mp_mark_key`, G) sends a `MARK`; each player has one, for 15
minutes. The player cards show each friend's layer and distance. Two multiplayer achievements count meetings (within
30 m in the same layer) and markers.

## Later layers

3. Friends in the world: an actor per friend, moved by the position and animation the room relays.
