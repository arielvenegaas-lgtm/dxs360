# Eden — Nintendo Switch game-library frontend for Xbox 360

Eden is a **frontend / game browser** for the Xbox 360 (RGH/JTAG), built on
libXenon. It scans a FAT32 USB stick for Nintendo Switch game files
(`.nsp`, `.xci`, `.nro`, `.nso`) and presents them in a controller-driven
library, inspired by the clean library experience of the Eden emulator.

> **This build does not emulate the Nintendo Switch.** It cannot boot any game.
> There are no working CPU/GPU/memory/firmware components, and none are faked.
> Selecting a game shows an honest "not implemented" notice.

## Why a frontend first
Switch emulation on a 2005 PowerPC console is extremely ambitious. Rather than
pretend, this milestone builds a solid, honest foundation: a real, polished
UI, real USB/FAT32 scanning, and real controller input. Emulation feasibility
can be investigated separately.

## Layout
```
eden/
├── Makefile              libXenon build (produces switch.elf32 -> switch.elf)
├── source/
│   ├── main.cpp          entry point + main loop
│   ├── app.{h,cpp}       screens, navigation, scanning
│   ├── ui/               theme, layout, all drawing
│   ├── input/            controller -> menu events (auto-repeat)
│   ├── filesystem/       USB + FAT32 access and directory scanning
│   ├── game/             game records + metadata
│   └── core/             honest capability model + real NSO/NRO header probe
├── tests/                host unit tests (loader), run in CI
└── dist/                 package template (roms/, keys/, assets/, README)
```

It **reuses** the verified libXenon renderer (`../xenon/source/gfx.cpp`),
controller input (`../xenon/source/input.cpp`) and the read-only FAT32 reader
(`../xenon/source/fat32.c`, extended with directory listing).

## Core status

There is still **no emulation**. `core/core.cpp` exposes a truthful capability
model (CPU/GPU/Memory/Firmware/Loader/Audio) and a feasibility table comparing a
Switch title's needs against the console's 3 PowerPC cores and 512 MB RAM; the
**System Status** screen shows both. `core/loader.cpp` does real, non-executing
work: it decodes the header of an **NSO** or **NRO** module (segment sizes,
build id) with strict bounds checks, so Game Info can show a module's actual
layout without ever mapping or running code.

## Boot and storage

Eden boots exactly like the game build: `Gfx_Create -> xenon_make_it_faster ->
Input_Init -> UI loop`. It deliberately does **not** touch USB mass storage at
boot, because libXenon's USB mass-storage probe can hang the console when the
ELF is launched from Aurora. Storage is mounted lazily the first time you scan
(open **Library** and press **A**, or use **Rescan Games**). If scanning hangs
on your console, launch `switch.elf` from XeLL instead of Aurora.

## Build
```
docker run --rm -v "$PWD":/app -w /app free60/libxenon:latest bash -lc "make -C eden"
```
or via GitHub Actions (`.github/workflows/build-eden.yml`), which also packages
the release ZIP.

## Legal
Ships no games, firmware, keys or copyrighted art. GPL-3.0-or-later.
Inspired by Eden (https://git.eden-emu.dev/eden-emu/eden); not affiliated.
