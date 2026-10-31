# Achievements

The port checks a set of achievements against the game's own progress and shows a notice when one unlocks. The set is
the player's: the achievements editor makes, changes and orders them, with their names, icons, rarities and conditions.

## How it works

The game keeps its progress in GameData flags. `progress.sav` of each save slot holds them by hash (Murmur3, 32-bit,
seed 0, of the flag's name): after a 0x20-byte header (magic `0x01020304`, the GameDataList revision, the offset of
the data area) come `(hash, value)` pairs in sections, each opened by a pair `(0, type index)`. Bool, Int, UInt, Float
and Enum values sit in the pair; arrays and strings at the value's offset in the file (a count, then the items).
`GameData/GameDataList.Product.<n>.byml.zs` (a big-endian BYML version 7) lists every flag with its type, and a
`Struct` entry lists its members, so a family of flags can be counted without knowing each member's name:

| Family | Members | What a member is |
|---|---|---|
| `DungeonState` (enums: Hidden, Appear, Open, Enter, Clear) | 152 | a shrine |
| `CheckPoint_IsDisplayFoliage` | 120 | a lightroot, lit |
| `IsOpenCannon` | 15 | a Skyview Tower, opened |
| `IsGetCaveMasterMedal` | 241 | a cave's Bubbul gem, taken |
| `IsVisitLocation` | 1423 | a named place, visited |
| `IsTreasureBoxOpen` | 222 | a tracked chest, opened |

A quest's progress is the enum `Step_<quest>`, whose last value is `Complete`.

1. `tools/achievements.py` holds the condition catalogue (Portuguese labels), the default set of 52 achievements and
   their emblem icons, and the compiler: it resolves each condition against the install's GameDataList and writes
   `achievements/compiled.tsv` (flag hashes) and `achievements/icons/<id>.rgba` (64x64) next to `totk.exe`. A name that
   is not a flag of the expected type stops it with that name.
2. The setup runs `achievements.py defaults <install>` in its prepare step: the default set when the folder has none,
   then the compile. The player's own set stays across updates and is compiled again for the new release.
3. `runtime/achievements.c` reads the compiled file at start. After every save commit (`runtime/fs_host.c`
   `commit_run`) and once when the save storage comes up, it reads the slot whose `progress.sav` was written last,
   records what became true in `profile/achievements.tsv` and queues a notice. Unlocks are kept for good, whatever
   save is loaded later.
4. `runtime/ui/menu.cpp` shows the notice at the top of the screen (`TOTK_ACH_POPUP`, *Achievement notices* on the
   Screen tab) and the *Achievements* tab with every achievement, its progress and its unlock date. The icons ride in
   the font atlas as custom rectangles.

The notice comes with the save after the feat, not at the feat itself: the game autosaves often, so that is seconds to
a few minutes.

## The editor

`tools/achievements_editor.py --dest <install>` (Tk, standard library only). The setup makes the shortcut *Editor de
Conquistas* in the install folder and in the Start menu (`installer/steps.py` `editor_shortcuts`), pointing at the
release's `pythonw.exe`. A picture is imported from any file Tk opens, or a JPEG or BMP through Windows' decoder (the
game's album photos are JPEG), kept at up to 512 pixels in `achievements/sources/`, and framed in the rarity's
medallion with a zoom and an offset. *Salvar e aplicar* writes `achievements.json`, the icons, and compiles; the game
reads them at its next start.

## Files in the install folder

| Path | Written by |
|---|---|
| `achievements/achievements.json` | the setup (default set) or the editor |
| `achievements/icons/<id>.png` | the editor, the compiler for an icon that is missing |
| `achievements/sources/<id>.png` | the editor (imported pictures) |
| `achievements/compiled.tsv`, `achievements/icons/<id>.rgba` | the compiler |
| `profile/achievements.tsv` | the game: the unlocked ids and when |
