# VibeDuke3D v1.1
## AKA jfduke3d-xbox

Duke Nukem 3D port for the original Xbox, based on [JFDuke3D](http://www.jonof.id.au/jfduke3d) by Jonathon Fowler. Built using the [nxdk](https://github.com/XboxDev/nxdk) open-source Xbox development kit.

This project was almost entirely vibe coded using Claude. We used JFDuke3D as the base and NXDK to get it working on Xbox. The result is a high performance, direct PC port with features like 720P 60 FPS gameplay, and hardware audio with 5.1 surround support.

## Installation

FTP or otherwise move the XBE file to the desired location on your Xbox and place a copy of `DUKE3D.GRP` next to it, then run.

## Features

- Hardware renderer, conversion of POLYMOST to NV2A
- OGG Vorbis music playback via stb_vorbis
- Hardware audio support featuring full 5.1 surround sound via optical
- Native 720p with auto-detection (480i/480p/720p), can be specified in config if detection fails
- Full Xbox controller support (dual analog sticks, triggers, vibration)
- Crosshair adjusts to hitscan position, making aiming more accurate (not perfect)
- Save/load game support
- Precaches common sprites to improve performance
- Cheat support (see below)
- Demo recording support, use the record demo option to save a replay that will show on the title screen
- User map support
- Gameplay Modifiers
- Workings mirrors, nightvision, and underwater tints
- Writable directory probing (supports both HDD and disc boot)

### Music

For music, extract the MID files from your GRP, convert them to OGG, and place in a `music/` folder next to the XBE file. There are scripts available in the repo to convert MIDI to OGG and further optimize OGGs. You can also copy the music folder straight from Megaton Edition.

### Controls

```
A                   - Jump
B                   - Use Item
X                   - Activate
Y                   - Crouch
Left Stick          - Move
Left Stick Click    - Kick
Right Stick         - Aim
Right Stick Click   - Third Person Toggle
DPAD Left/Right     - Select Item
DPAD Up             - Activate Jetpack
DPAD Down           - Activate Medkit
Right Trigger       - Shoot
Left Trigger        - Walk
Black/White         - Switch Weapons
Select              - Toggle Map
Start               - Pause
```

### Cheats

```
Up Up Down Down Left Right Left Right B A B A  - God Mode
Up Up Down Down Left Right Left Right A B A B  - Give Everything
Up Up Down Down Left Right Left Right A A B B  - Toggle Clipping
Up Up Down Down Left Right Left Right B B A A  - Teleport to Stadium
```

### Known Issues

- May have missed a couple spots for Duke voice positional audio correction, so it will play out of other channels than center on 5.1 (only known is KTIT mic easter egg).
- May still have minor graphical issues, but the ones I knew of have been fixed.
- Equipped RPG sprite needs to be aligned to the right on widescreen resolutions.  
- This will not work on emulators due to how rendering is handled and hardware audio!
- If you see any strange issues, please reach out to me on Discord and attach your log file (link below).

### Future Updates

- Mod support?
- Multiplayer and couch coop


### Screenshots

<img width="2880" height="2160" alt="image" src="https://github.com/user-attachments/assets/79f3e262-c413-4e8c-830d-a62fa6f518dc" /> 
<img width="2880" height="2160" alt="image" src="https://github.com/user-attachments/assets/6dd16cfb-c8bf-46aa-a057-60e501fb7ad9" />
<img width="2880" height="2160" alt="image" src="https://github.com/user-attachments/assets/2139cdd1-fc3e-4a94-a7c5-f2227ce906db" />
<img width="2880" height="2160" alt="image" src="https://github.com/user-attachments/assets/f1e80364-b391-4fd3-a26f-63fcd6d01072" />
<img width="2880" height="2160" alt="image" src="https://github.com/user-attachments/assets/2519ddf1-67e5-485b-89a3-fcdfaed2e43b" />


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

