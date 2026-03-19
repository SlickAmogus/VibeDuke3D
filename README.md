# VibeDuke3D v1.0
## AKA jfduke3d-xbox

Duke Nukem 3D port for the original Xbox, based on [JFDuke3D](http://www.jonof.id.au/jfduke3d) by Jonathon Fowler. Built using the [nxdk](https://github.com/XboxDev/nxdk) open-source Xbox development kit.

This project was almost entirely vibe coded using Claude. We used JFDuke3D as the base and NXDK to get it working on Xbox. The result is a high performance, direct PC port with features like 720P 60 FPS gameplay, and hardware audio with 5.1 surround support. 

## Features

- Hardware renderer, conversion of POLYMOST to NV2A.
- OGG Vorbis music playback via stb_vorbis
- SDL2 input
- Hardware audio including support for optical and 5.1 surround
- Xbox controller support (dual analog sticks, triggers)
- Save/load game support
- 480i/480p/720p/1080i auto-detection

## Development Prerequisites

- **MSYS2** with GNU Make (`pacman -S make`) — or Git Bash with Make available
- **[nxdk](https://github.com/SlickAmogus/nxdk)** — included as a git submodule (fork with S/PDIF audio patch)
- **clang/lld** — provided by nxdk, no separate install needed

After cloning, initialize the nxdk submodule:

```bash
git submodule update --init --recursive
```

## Building

```bash
bash build_xbox.sh
```

The build script handles the nxdk toolchain setup automatically — it sets `NXDK_DIR`, compiles Xbox-specific startup code, then runs `make -f Makefile.nxdk`.

Output: `bin/default.xbe` (~2.4 MB Xbox executable)

## Game Data Setup

You need a legitimate copy of Duke Nukem 3D or the shareware. Place the following on your Xbox HDD in the same directory as `default.xbe`:

- `DUKE3D.GRP` — Main game data file
- `music/` — (Optional) OGG Vorbis music files (use `convert_midi_to_ogg.sh` to convert from MIDI). Please note they should be the same name as the MID file, only with OGG extension. You can extract them from the GRP and different tools are available.

## Directory Layout

```
src/           Game logic (actors, menus, player, etc.)
jfbuild/       Build engine (renderer, file I/O, SDL layer)
jfmact/        Input/control library
jfaudiolib/    Audio library (mixing, music, sound effects)
xbox_compat/   Xbox compatibility shims (POSIX I/O, stubs, startup)
nxdk/          nxdk submodule (Xbox open-source SDK, S/PDIF fork)
```

## License

GPLv2 — see GPL.TXT


## Support

Feel free to message me on Discord @KushAstronaut or join my server (but nobody is in it so I may not notice):\
https://discord.gg/U29t39WR73 - I am also in most duke or xbox related discord servers

If you like what I do:\
[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/F1F3K8V3B)

