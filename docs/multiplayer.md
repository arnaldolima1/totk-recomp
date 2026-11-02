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
| game | `POS` | x, y, z: the player's position in metres (centimetres given), yaw: the way Link faces (radians from +z towards +x, empty when unread), t: the game's clock in ms, anim: the animation Link plays (1.4.3, empty when unread), quat, sfx, body: his real body pose (1.4.3, base64url, empty when unread), gear: what he carries (1.4.3, per item `actor@bone:entries`, `|` between items); while moving, turning or changing animation, else each second (one each 50 ms at most) |
| game | `DUEL` | `1` or `0`: the duel on or off in this game |
| game | `MARK` | x, y, z: a marker where the player stands |
| game | `ACT` | `item`, actor: an item Link got for the first time (1.4.3, its `IsGet.<actor>` flag); `quest`, quest, step, the step it left: a quest's step changed (1.4.3, its `Step_<quest>` enum); `hit`, attacker, quarter hearts / `duel`, attacker / `block`, attacker: a friend's duel strike hit, won or was stopped by the shield; a few a second at most |
| game | `QUESTS` | quest:step pairs of numbers, comma-separated: the quests under way or done (their place in `profile/quests.tsv`, the step's place among NotReady, the quest's steps, Complete); when it changes, every 2 s at most |
| game | `PING` | (every 2 s: the round trip to its `PONG` is the ping shown over the friends' list; the room drops a game silent for 120 s) |
| room | `WELCOME` | protocol, room name |
| room | `ERROR` | code (`version`, `password`, `name`, `full`), text |
| room | `ROSTER` | count, then per player: name, place and the six figures of `STATUS` |
| room | `SLOTS` | count, then per player: name, slot (0 green, 1 red, 2 blue, 3 purple); right after each `ROSTER` |
| room | `CHAT` | player, text |
| room | `ACH` | player, achievement id, name, rarity |
| room | `VOICE` | player, the frame as received (to everyone but its sender) |
| room | `POS` | player, x, y, z, yaw, t, anim, quat, sfx, body, gear (to everyone else; the last place alone to a player who joins) |
| room | `DUEL` | player, `1` or `0` (to everyone else; everyone's to a player who joins) |
| room | `MARK` | player, x, y, z (to everyone, its player too; markers under 15 minutes old to a player who joins) |
| room | `QUESTS` | player, their list (to everyone else; everyone's to a player who joins) |
| room | `EVENT` | kind (`join`, `leave`, `challenge`, `shrines`, `lightroots`, `towers`, `koroks`, `hearts`, `death`, `warp`, `chest`, `item`, `quest`, `hit`, `duel`, `block`; an `item` event's step is the actor, a `quest` event's quest:step:the step it left), text (Portuguese), player, step, total, unix time, and `old` on the last 60 events a player gets when they join |
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
game sends it in `POS` lines with the way Link faces (`game_player_pose`: the third column of the actor's rotation at
+0x2c0) and its own clock, and keeps its friends' by name for six seconds. Of each friend the last 24 places are kept
(`net_mp.c` `place_at`): a friend is shown 90 ms behind their newest place, between the two real places around that
moment on their clock (the offset to it follows the quickest delivery), so the figure takes the path they took, up a
slope or over a jump, instead of a guess that cuts through the ground. Past the newest place it goes on along the
last step for at most 200 ms, then stands. Both the game and the room turn Nagle's algorithm off (`TCP_NODELAY`): with
it a small line waits until the one before is acknowledged, which Windows delays up to 200 ms, and positions and voice
frames would come in bursts.

On 1.4.3 a `POS` line also carries what Link does, read from the game (all `runtime/frame_rate.c`, found with the
research probe `runtime/anim_probe.c`, `TOTK_ANIM_PROBE`):

- his whole rotation as a quaternion (`game_player_quat`, the actor's 3x3 at +0x2c0): a back flip or a side jump turns
  the actor over, which angles about each axis cannot carry past 90 degrees;
- the animation he plays and its frame (`game_player_anim`): the animation player at `[[[actor+0x238]+0xc8]+0x118]`,
  its first slot's FSKA at +0x80 (blended layers follow every 0xb0) and that slot's frame at +0x58. The game starts some
  animations past their first frame and plays them at its own pace, so the figure plays the frames the friend's game
  shows rather than counting its own;
- the voice clips he starts (`game_player_voices`): his sound link handles, four a block in blocks reached from
  +0x608, point at the asset calls of the game's `SLink2` file, whose key names are the clips of `Player_Voice`
  (`PV000_02`);
- his body as the game draws it (`game_player_body`): the model unit `[actor+0x238]` holds the body's model
  (`Link_RaulSkin`) at +0x3e0, whose `[+0x458]+0x8` is that model's skinning palette for the frame, per
  `matrix_to_bone` entry a row-major 3x4 (the bone's pose times its inverse bind; the rigid entries after the smooth
  ones are the pose itself). It is the final pose: the layers blended (the shield held up, the bow drawn), the rag doll,
  the feet set on the ground. The palette is in world axes about a render origin on a 10 m grid near Link; the game
  takes it into Link's actor space and sends the rotations of 58 entries (spine, arms, hands, head, waist, belts, legs;
  not the face) as int16 quaternions with the translations of `Spine_1` and `Waist` (`link_figure.c`
  `link_body_encode`, 636 characters).

A game sends a line every 50 ms while it knows the frame. With a body pose the figure takes those bones from it
(`link_figure_pose_body`): tools/link_figure.py writes which figure bone each palette entry moves (`PALM` in
link-figure.bin), the rotations go on the figure's own bind, and the bones the body's palette lacks (the Zonai forearm,
whose model has a palette of its own, and the hair) keep the animation's local transforms under them. Without one (an
older game, another version) the figure plays the animation as below. The figure (`ui/menu.cpp` `figure_animate`) plays that
animation; it keeps the speed-picked gait for the game's turning leans (`*_Curve*`) and ignores its pose and face
layers (`A_Pose_*`, `Face_*`, `Voice_*`), which the game blends over another animation and which lead the slot now and
then. Link's root bone carries the animation's own motion, which the game applies to the actor: the figure holds the
root where its curves start (`link_figure.c` `pose_worlds`) and adds only the turn the actor did not make
(`link_figure_root_yaw`). Knocked down, Link tumbles as a rag doll that no animation describes; while the game shows
its one-frame `Rolldown_*` reference the figure lies as the matching getting-up animation begins.

What Link wears is read live too (`game_player_armor`: the model parts listed at `[actor+0x168]+0x2e0`) and sent in
`LOOK` lines when it changes; another game version sends what its last save holds.

The room keeps a log (`tools/mp_server.py` `Room.event`): who joined and left, the group's challenges, a player's
figures going up between two `STATUS` lines (a shrine, a lightroot, a tower, Korok seeds, heart containers; the figures
come from the evaluated save, so these follow the game's saves) and what their Link's animation in `POS` lines shows
(`Nml_Die*` and `Swim_Die`: died; `WarpHeightIn`: fast travel; `Nml_Get*`, Link holding up what a chest gave: opened a
chest), and the items a player's game says they got for the first time (`ACT item`; a power when the actor is one of
`POWERS`: Ultrahand, Recall, Autobuild, the camera). Items show by the name the game gives them in the menu's language (English, Brazilian
Portuguese or Russian; `profile/item-names.tsv`, which the installer reads from the game's texts with
`tools/item_names.py`). A newer game words the kinds it knows in the menu's
language (`net_mp.c` `event_text`) and lists them, with friends' unlocks, on the Multiplayer tab's Log page
(`ui/menu.cpp` `mp_page_log`); the past events a newcomer gets show there without notices.

On 1.4.3 the game reads its bool flags while it runs (`runtime/game_flags.c`, found with the research probe
`runtime/gmd.c`): a global of the main module leads to the flag manager's bool table (21414 flags of 24 bytes, the
value at byte 22) and its hash map (hash % capacity, linear probing). Flag names are MurmurHash3 (x86, 32 bits, seed 0)
of names such as `IsGet.<actor>`, set the first time Link gets that actor's item; the game hashes every actor of its
RomFS so (`game_flag_item`). A watcher thread keeps the changes for the room's thread, a reload's burst of hundreds as
one mark. `TOTK_FLAG_LOG=1` logs every change, the other value tables' too, to `<profile>/flag-log.tsv`.

Players follow the story together without sharing it: each keeps their own save, and the room shows where each
stands. The game's quests are listed in its RomFS (`RSDB/Challenge`, the quest table, with each quest's steps; titles
in `ChallengeMsg/Info_<quest>.msbt`), which the installer turns into `profile/quests.tsv` (`tools/item_names.py
--quests`). A quest's progress is the enum flag `Step_<quest>` (the manager's enum table, 56-byte entries, the value at
+28: the hash of the step's name): NotReady, Ready (offered, not taken), its steps, Complete. Once a second the game
reads them all; a quest leaving NotReady or Ready is begun, a step further moved on, Complete done (`ACT quest`, the
Log), and its whole list goes in `QUESTS` lines for the Quests page, where each player has a column (`ui/menu.cpp`
`mp_page_quests`): the quests in three sections (the main story, side quests, the rest), each with a bar per player
in their colour and the adventure log's text at this game's step, or at the furthest friend's (`profile/quest-steps.tsv`,
the text the game's log shows at each step).

Combat groundwork (1.4.3, `runtime/frame_rate.c`): Link's life is a count of quarter hearts at `[actor+0x600]+0x248`,
`+0x298`, found by making an enemy hit him (`runtime/actor_spawn.c`, `TOTK_SPAWN=<actor>@<seconds>`, which asks the
game's `ActorMgr::requestCreateActorAsync` for an actor in front of him) while `anim_probe.c` (`TOTK_ANIM_PROBE=life`)
kept the words that followed the GameData flag `PlayerStatus.Life`. That flag only mirrors the life: the game writes it
as the life changes and never reads it back. `game_player_life_set` writes the life itself, and the hearts follow
(`TOTK_LIFE_SET` tests it).

The duel (setting `mp_duel`, off by default; a `DUEL` line tells the room): each friend with it on can take life from
this game's Link with their strikes (`runtime/net_mp.c` `duel_tick`). A strike begins when the friend's animation in
their `POS` lines is an attack (`Sword_Attack`, `Lsword_Attack`, `Spear_Attack`, `Sword_Blunt_Attack`; a charged one
twice, from the air half again); 150 ms on it hits when they stand within reach (1.9 m for a sword, 2.4 m for a
two-handed one, 2.8 m for a spear), at most 2 m above or below, facing Link. Link's raised shield (a `*_Guard*` layer
of his animation, `frame_rate.c` `game_player_anim_layers`) toward the friend stops it (`ACT block`). A hit goes
through the game's own damage step (`runtime/hit_hook.c`, the patch set `hithook`): Link's damage step
(main+0x2f9910) gathers the attacks that touched him into a hit list and hands it to main+0x315dd4, which turns each
record into damage, the reaction animation and the knock-back; the patch replaces that call with a host function that
adds a record built like an enemy's (a Chuchu's, read with `TOTK_ANIM_PROBE=lifewatch`, which watches the life word
with the debug registers) for the strike's quarter hearts and direction, then makes the call. No attacker object is
needed. The step runs for every actor of the player's kind on job threads at once, so the record goes in only on the
call for Link (`x19`). A strike that would take the last quarter leaves one, the life comes back full 1.2 s later and
the room is told the friend won (`ACT duel`): nobody really dies. `TOTK_HIT_TEST=<seconds>` strikes Link from 1.5 m at
those world times.

What Link carries is sent too (`runtime/game_gear.c`, the `POS` line's gear field): the weapon, shield and bow he
has equipped (the GameData pouch: `Pouch.<Weapon|Shield|Bow>.EquipIndex` is the slot in use, an int array, and
`Pouch.<kind>.Content.Name` its actor, a String64 array; `game_flags.c` reads both tables), and the arrow while a
layer of his animation is `Bow_Attack_Charge`. Each item is an actor whose model object (its name at `+0x90`, the
model file's, which the actor's pack's ModelInfo gives: `Weapon_Sword_168`, the Wooden Stick, shows
`Weapon_Sword_103`) has a skinning palette at `[+0x458]+0x8`: the item as the game draws it this frame, in the hand or
on the back. No fixed path from Link's actor leads there, so a background search finds the model object by its name's
text in the heap and keeps it while it matches. The palette's entries go in the field relative to the bone the item
hangs from (the right or left wrist or the upper back, whichever is nearest), so the figure's own bone carries it: a
bow's string bends by its own entries. `profile/link-armor.bin` holds the weapons' models too (the installer's
`link_figure` step).

The ping over the friends' list is the round trip of a `PING` (every 2 s) to its `PONG`.

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
list at the top right gives each friend's distance and layer. The layer comes from the height (`mp_layer`:
sky over 900 m, depths under -200 m). The mark key (`mp_mark_key`, G) sends a `MARK`; each player has one, for 15
minutes. The player cards show each friend's layer and distance. Two multiplayer achievements count meetings (within
30 m in the same layer) and markers.

## Figures

Each friend is drawn in the world as a textured, animated Link of the game's own models, into the game's G-buffer
(`runtime/vk_figure.c`, `docs/maxwell.md` section 9), so the game lights, shadows and fogs them like its own
characters. The figure, `profile/link-figure.bin`, is built at install time from the player's own RomFS
(`installer/steps.py` `link_figure`, `tools/link_figure.py`): the parts' `.bfres.mc` models through McSharp's
MeshCodec decoder (the release's `bin/mcsharp`), the textures from TexToGo, and the curves of all 1560 of Link's
skeletal animations (`Player_Animation`, about 35 MB), which the runtime evaluates at the frame it shows. Without it
friends show as outlines.

Their sounds come from the game too: `profile/link-sounds.bin` (`installer/steps.py` `link_sounds`,
`tools/link_sounds.py`) holds Link's footsteps on grass, soil and stone, jumps and landings, sword swings, drawing and
sheathing, climbing, swimming, the paraglider, the roll and his 337 voice clips, decoded from the RomFS's BARS files
(DSP ADPCM). `ui/menu.cpp` `figure_sounds` plays a step when a figure's ankle comes down to the ground (it rests at
0.12 m in every gait), the other sounds as their animations start, and each voice clip the friend's game names, from
the friend's side and quieter with the distance (silent beyond 50 m); `runtime/friend_sfx.c` mixes them over the
game's output.
