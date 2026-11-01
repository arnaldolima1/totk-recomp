# TotK Recomp

**TotK Recomp** is a static recompilation of *The Legend of Zelda: Tears of the Kingdom* (Nintendo Switch, 2023) into a native Windows x86-64 application ([docs/architecture.md](docs/architecture.md)).

> [!NOTE]
> **This repository is a fork.** TotK Recomp is created and maintained by **[valentderah](https://github.com/valentderah)** in [valentderah/totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp): the recompiler, the runtime, the Vulkan renderer and the installer are that project's work, with the contributors credited there and in [Credits](#credits). This fork, by [arnaldolima1](https://github.com/arnaldolima1), follows its releases and adds the features listed in [This fork](#this-fork). Problems that also happen on the original belong in its issue tracker.

## This fork

Added on top of the original, merged with each of its releases:

- **Launcher** (`launcher/`): guided install and updates from GitHub, `totk.ini` settings with key remapping, mods in the emulator layout, Brazilian Portuguese interface
- **Brazilian Portuguese** for the in-game menu, and the game's own language shown as Português (Brasil) with the launcher's translation
- **Menu look**: a panel from the left with tab icons and the keys on its bottom line
- **Achievements** ([docs/achievements.md](docs/achievements.md)): 64 achievements read from the game's own save progress, a notice with a chime, an *Achievements* tab, a platinum trophy, an editor for making your own with imported pictures, and unlocks that count only for progress made in play
- **PlayStation button prompts** ([docs/controls.md](docs/controls.md)): ✕ ○ □ △ in place of the letters, drawn by the setup from the game's own files
- **DualSense**: adaptive triggers and the light bar
- Smoother mouse camera, borderless window and a notice while shaders compile
- Faster updates: the setup keeps the compiled game code and reuses what did not change

> [!IMPORTANT]
> **No game files are included.** You need your own legally obtained copy of *Tears of the Kingdom*, dumped from your own console: the base game and the 1.2.1 or 1.4.3 update. Instructions for obtaining those files will not be provided.
>
> The game is built from your files on your machine, so the result contains the game. It is for your own use. Do not share it.

## Status

Early development. The game is playable on Windows; visual bugs and crashes are still expected.

Report problems as issues, following [CONTRIBUTING.md](CONTRIBUTING.md).

## Features

- Native code, not emulation
- Game updates 1.2.1 and 1.4.3, with saves that carry over between them
- 60 fps by default; up to 144 fps or the monitor's refresh rate (experimental)
- Custom resolutions (1080p, 2k...), supersampling and AMD FSR 1 upscaling
- 16:10 (Steam Deck, handhelds), 21:9 and 32:9 (ultrawide) game resolutions, with the camera and the interface in that shape
- In-game settings menu (F1) and performance overlay (F10)
- Keyboard, mouse and gamepad with remapping, rumble and gyro
- Xbox button prompts: a layout that keeps the Switch button positions and shows the Xbox labels
- PlayStation button prompts (this fork)
- Achievements with an editor (this fork)
- Shaders prepared at install instead of during play

## Planned

- Linux and the Steam Deck
- Mods made for emulators: unpack a mod into a `mods` folder
- VRR

## Installing

Requirements: Windows 10 or later, an x86-64 CPU with AVX2, a GPU with Vulkan 1.3 and `VK_EXT_external_memory_host`, about 36 GiB free during setup (20 GiB after).

With this fork's launcher: download `TotK.Recomp.Launcher_<version>_x64-setup.exe` from the [latest release](../../releases/latest), install it, and let it download and run the setup; it also offers the updates. By hand:

1. Download `TotK-Setup-<version>-windows.zip` from a `game-v<version>` release on [Releases](../../releases) and unpack it.
2. Start `TotK-Setup.exe`. If SmartScreen warns about an unsigned app, choose *More info*, then *Run anyway*.
3. Pick your game files, the language and an install folder, and press *Start*. The setup builds the game once.
4. Play `totk.exe` from the install folder.

To update, run a newer `TotK-Setup.exe` on the same folder; saves and settings are kept. Settings are in the in-game menu and in `totk.ini` ([totk.ini.example](totk.ini.example) lists every key). Controls: [docs/controls.md](docs/controls.md).

## Credits

- **[valentderah](https://github.com/valentderah)**: creator and maintainer of TotK Recomp ([valentderah/totk-pc-recomp](https://github.com/valentderah/totk-pc-recomp)), and the contributors to that repository, among them Lucas-blip-png (colour profiles, field of view), shenjackyuanjie (the 1.1.1 update, the shader walk), wdiasjunior (the Linux port and Steam Deck), lcd1232 (the Linux installer's text) and JieML1100 (the equipment-switch fix)
- **[arnaldolima1](https://github.com/arnaldolima1)**: this fork's launcher, achievements, PlayStation prompts, Brazilian Portuguese and the other changes in [This fork](#this-fork)
- **UltraTears**: the look of the in-game menu and the fork the launcher grew from

TotK Recomp builds on the projects below. What was taken from each, with its license, is listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

- [LAS Recomp](https://github.com/peiikai/LASRecomp)
- [MacNativeGames](https://github.com/ly05010419/MacNativeGames)
- [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) and [nxnative](https://github.com/seedds/nxnative)
- [SavageWindRecomp](https://github.com/Revivedx/SavageWindRecomp)
- [Ryujinx](https://github.com/Ryujinx/Ryujinx)
- [deko3d](https://github.com/devkitPro/deko3d)
- [hactool](https://github.com/SciresM/hactool)
- [SDL 3](https://libsdl.org), [Dear ImGui](https://github.com/ocornut/imgui), [AMD FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR), [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator), [libopus](https://opus-codec.org), [libvpx](https://chromium.googlesource.com/webm/libvpx), [Zstandard](https://github.com/facebook/zstd)
- [Switchbrew](https://switchbrew.org) and the Switch homebrew community

## License

The project's own code is under the [MIT License](LICENSE); third-party code keeps its own license ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). The license grants nothing for Nintendo's game.

TotK Recomp is an independent, non-commercial fan project, not affiliated with or endorsed by Nintendo. *The Legend of Zelda*, *Tears of the Kingdom* and *Nintendo Switch* are trademarks of Nintendo.

## Building from source

[docs/building.md](docs/building.md) lists the toolchain and the steps. Everything the build writes stays under `build\` and `generated\`, which git ignores; run `tools/check_content.py` before a commit. Testers: [docs/tester-guide.md](docs/tester-guide.md).

Most of the code and documentation is written by AI coding agents (Claude Code) under the maintainer's direction; the maintainer plays every build and decides what is merged. [AGENTS.md](AGENTS.md) holds the rules they work under.
