# VibeDuke v1.0
# AKA jfduke3d-xbox

Duke Nukem 3D port for the original Xbox, based on [JFDuke3D](http://www.jonof.id.au/jfduke3d) by Jonathon Fowler. Built using the [nxdk](https://github.com/XboxDev/nxdk) open-source Xbox development kit.

This project was almost entirely vibe coded using Claude. We started from scratch, using just JFDuke3D as a base and NXDK as the guy. Claude acted as the developer while I designed and did QA. After about two or three days,
this is the end result. A modern, fully functional, and playable port of Duke Nukem 3D for OG Xbox with nohthing cut.

## Features

- Software renderer (Build engine classic mode)
- OGG Vorbis music playback via stb_vorbis
- SDL2 audio and input
- Xbox controller support (dual analog sticks, triggers)
- Save/load game support
- 480i/480p/720p/1080i auto-detection

## Devevelopment Prerequisites

- [nxdk](https://github.com/XboxDev/nxdk) installed (default: `/c/Claude/nxdk`, set `NXDK_DIR` to override)
- MSYS2 or Git Bash on Windows
- GNU Make (MSYS2: `pacman -S make`)

## Building

```bash
bash build_xbox.sh
```

Output: `bin/default.xbe`

## Game Data Setup

You need a legitimate copy of Duke Nukem 3D. Place the following on your Xbox HDD in the same directory as `default.xbe`:

- `DUKE3D.GRP` — Main game data file
- `music/` — OGG Vorbis music files (use `convert_midi_to_ogg.sh` to convert from MIDI). Please note they should be the same name as the MID file, only with OGG extension.

## Directory Layout

```
src/           Game logic (actors, menus, player, etc.)
jfbuild/       Build engine (renderer, file I/O, SDL layer)
jfmact/        Input/control library
jfaudiolib/    Audio library (mixing, music, sound effects)
xbox_compat/   Xbox compatibility shims (POSIX I/O, stubs, startup)
```

## License

GPLv2 — see GPL.TXT


