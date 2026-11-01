# Multiplayer

Up to four friends play side by side, each in their own world, as the four Links of Four Swords: a room shares
everyone's progress, their achievement unlocks, a text chat and a voice chat, keeps group challenges the players reach
together, and shows each friend in the world as a Link whose hair is their room colour (green, red, blue, purple). No
game state crosses between the games.

Rooms open once the prologue is over: the game joins only after Link has woken on the Great Sky Island in the newest
save (the flag `SageOfSky_IsOpenShutterInWakeUpRoom`, read by `runtime/achievements.c` with the other save figures;
`runtime/net_mp.c` `island_reached`). Until then the tab says so (state `MP_LOCKED`); a new game started later leaves
the room again.

## Playing

1. One player opens the room on the launcher's *Multijogador* tab (`launcher/src-tauri/src/room.rs`, which runs the
   release's `tools/mp_server.py` and lists the addresses to share: home network, Radmin, Tailscale, ZeroTier), or
   with the *Servidor Multijogador* shortcut the setup puts in the install folder and the Start menu (the same script
   in a console window). The tab shows only for a release that has the server. The same tab sets the game's name,
   address and password and tests whether a room answers. Friends in
   another house reach it through a VPN that joins the machines (Radmin VPN, Tailscale, ZeroTier) or a forwarded TCP
   port (5577 by default). The same script runs on any server with Python 3.10 or later:
   `python3 mp_server.py --port 5577 --password SECRET --name "My room"`.
2. Every player opens the menu (F1) on the *Multiplayer* tab, sets a name, the server (`address:port`) and the room's
   password, and presses *Connect*. The page shows the room's four slots in their colours with who holds each. The settings are `mp`, `mp_server`, `mp_name` and `mp_password` in `totk.ini`.
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
| room | `SLOTS` | count, then per player: name, slot (0 green, 1 red, 2 blue, 3 purple); right after each `ROSTER` |
| room | `CHAT` | player, text |
| room | `ACH` | player, achievement id, name, rarity |
| room | `VOICE` | player, the frame as received (to everyone but its sender) |
| room | `POS` | player, x, y, z (to everyone else; the last of each to a player who joins) |
| room | `MARK` | player, x, y, z (to everyone, its player too; markers under 15 minutes old to a player who joins) |
| room | `EVENT` | kind (`join`, `leave`, `challenge`), text |
| room | `CHALLENGES` | count, then per track: figure, the room's total, tiers reached, four targets |
| room | `CHALLENGE` | figure (or `done`), the room's total, target, text (the closest tier, for older games) |
| room | `PONG` | |

A room holds four players (`MAX_PLAYERS`); a fifth gets `ERROR full`. Each player gets a slot when they join: the
one they had last time when it is free, else the lowest free one (kept in the room's state by name). The slot picks
the player's colour in every game: their name, dots, chat lines and their figure's hair (`runtime/ui/menu.cpp`
`kSlotCol`). Games ignore message kinds they do not know, so an older game in a newer room just keeps colours from
the name.

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

Friends show on the game's own minimap and in the world (`runtime/ui/menu.cpp` `radar_draw`, setting `mp_radar`).
The overlay takes the game's camera from the G-buffer pass (`runtime/vk_exec_draw.c` `cam_take`: the 2368-byte block
at vertex uniform 1, whose layout MacNativeGames documents) and projects world positions on the window
(`runtime/gfx_present.c` `gfx_world_to_screen`). In the world a friend is a see-through Link-like figure of our own drawing
(`draw_figure`) with name and distance (`world_tags`); behind the scene it fades to a faint outline and keeps its name.
The depth test (`runtime/vk_occl.c`) copies the G-buffer depth under three points of each figure (feet, chest, head)
once that pass is done, reads it back when the submission completes, and compares the scene's distance with the
friend's; it runs only while friends are drawn (`TOTK_SPEED_PROBE=1` logs it, with the player's own chest as a check); on the minimap (`minimap_dots`) a dot with their initial, hollow with an arrow when in another layer, and
markers are diamonds. The minimap turns with the camera and shows about 1 m per pixel of a 1440-row picture, its centre
0.1819 and 0.1667 picture heights from the right and bottom edges (measured by walking a known distance on 1.4.3). A
list at the top right gives each friend's distance and layer. Positions glide between the 200 ms updates
(`smooth_positions`). The layer comes from the height (`mp_layer`:
sky over 900 m, depths under -200 m). The mark key (`mp_mark_key`, G) sends a `MARK`; each player has one, for 15
minutes. The player cards show each friend's layer and distance. Two multiplayer achievements count meetings (within
30 m in the same layer) and markers.

## Figures

Each friend is drawn in the world as a textured, animated Link of the game's own models, into the game's G-buffer
(`runtime/vk_figure.c`, `docs/maxwell.md` section 9), so the game lights, shadows and fogs them like its own
characters. The figure, `profile/link-figure.bin`, is built at install time from the player's own RomFS
(`installer/steps.py` `link_figure`, `tools/link_figure.py`): the parts' `.bfres.mc` models through McSharp's
MeshCodec decoder (the release's `bin/mcsharp`), the textures from TexToGo, and the walk, run, dash and idle
animations. Without it friends show as outlines.
