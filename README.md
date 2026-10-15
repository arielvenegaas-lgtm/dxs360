# Eden — Nintendo Switch emulator for Xbox 360

[![build-eden](https://github.com/arielvenegaas-lgtm/dxs360/actions/workflows/build-eden.yml/badge.svg)](https://github.com/arielvenegaas-lgtm/dxs360/actions/workflows/build-eden.yml)

An open-source **Nintendo Switch emulator for the Xbox 360** (RGH/JTAG),
built on [libXenon](https://github.com/Free60Project/libxenon) and inspired by
the [Eden](https://git.eden-emu.dev/eden-emu/eden) emulator. It reads your own
game files from a USB stick and presents them in a clean, controller-driven
library, with the emulation core developed behind an honest stub.

> **Project status — be aware:** the **game-library frontend is complete**, but
> the **emulation core is not implemented yet**. In the current build Eden
> browses and lists your games; it does **not** execute them, and no game can
> boot. Nothing is faked or simulated — the loader reports honestly that
> emulation is not available. Running Switch titles on a 2005 PowerPC console is
> a very ambitious target; this repository is built in honest, verifiable steps.

---

## What works today

- Boots on Xbox 360 via libXenon (`switch.elf`), draws to the Xenos display and
  reads a wired controller.
- Scans a FAT32 USB stick for Nintendo Switch game files
  (`.nsp`, `.xci`, `.nro`, `.nso`).
- Presents them in a game library with titles, extensions, file sizes and a
  detected/supported indicator.
- Home, Library, Game Info, Settings, About and in-game menu screens.
- Rescan, grid-columns, show-extensions and scan-on-start options.
- Runs its USB mass-storage probe **before** starting the pad driver, so a
  controller is never dropped by the device scan.

## What does **not** work yet

- **Emulation.** No CPU, GPU, memory or firmware components. `Core_CanBoot()`
  is always false and the loader tells you so.
- Bootable game execution of any kind.

---

## Download and run

1. Open the **Actions** tab and pick a green run of
   **`build-eden (Switch frontend ELF + package)`**.
2. Download the **`eden-frontend`** artifact (or **`eden-switch-elf`** for the
   bare ELF) and unzip it. You get an `eden/` folder.
3. Copy the **`eden`** folder to a FAT32 (MBR) USB stick.
4. Optionally drop your own game files into `eden/roms/`.
5. On the console, open **Aurora → File Manager**, browse to
   `eden/switch.elf` and press **A** (or launch it from XeLL).

The package layout is:

```
eden/
  switch.elf     the executable
  README.txt     usage notes shipped with the release
  LICENSE.txt    GPL-3.0
  roms/          your own game files go here
  keys/          your own prod.keys (unused for now)
  assets/        icons / backgrounds / fonts (placeholders)
```

---

## Controls

| Input | Action |
|---|---|
| D-pad / left stick | Navigate |
| **A** | Select / open |
| **B** | Back |
| **Start** | Menu (Library / Settings / About / Rescan / Close) |
| **LB / RB** | Previous / next page |

Use a **wired** controller (or the Xbox 360 wireless PC receiver). A
play-and-charge cable only charges.

---

## Build

CI builds and packages Eden on every push with no Microsoft XDK required. To
build locally with Docker:

```sh
docker run --rm -v "$PWD":/app -w /app free60/libxenon:latest \
  bash -lc 'make -C eden'
# -> eden/switch.elf32   (packaged as eden/switch.elf)
```

The build uses `-O2 -fno-tree-vectorize`: the Xenon's VMX128 core mis-executes
the standard AltiVec that GCC emits at `-O3`.

The `build-eden` workflow verifies the output is a genuine 32-bit big-endian
PowerPC ELF before packaging the release ZIP.

---

## Layout

```
eden/
  Makefile                 libXenon build (-> switch.elf32 -> switch.elf)
  source/
    main.cpp               bare-metal entry + main loop
    app.{h,cpp}            screens, navigation, scanning
    ui/                    theme, layout, all screen drawing
      theme.h  layout.h  ui.h  ui.cpp
    input/                 controller -> menu events (auto-repeat)
      nav.h  nav.cpp
    filesystem/            USB + FAT32 access and directory scanning
      fs.h  fs.cpp
    game/                  game records + roms/ scanning
      library.h  library.cpp
    core/                  reserved for the emulation core (honest stubs)
      core.h  core.cpp
  dist/                    package template (roms/, keys/, assets/, README)
  LICENSE.txt              GPL-3.0
```

Eden reuses the verified libXenon renderer (`xenon/source/gfx.cpp`), controller
input (`xenon/source/input.cpp`) and the read-only FAT32 reader
(`xenon/source/fat32.c`), extended with directory enumeration.

---

## Roadmap

1. ✅ Frontend: game library, screens, controller navigation, USB/FAT32 scan.
2. ✅ Release packaging + CI verification (genuine PPC ELF + ZIP).
3. ⏳ Investigate emulation feasibility on the Xbox 360 (CPU translation,
   memory budget, GPU) — this is the hard, open-ended part.
4. ⏳ A bootable core behind `Core_*`, if and when it proves viable.

---

## Legal

- Ships **no** games, **no** firmware, **no** `prod.keys` and **no** copyrighted
  box art. You must supply your own, legally obtained files. Eden draws neutral
  placeholders rather than inventing covers.
- Licensed **GPL-3.0-or-later** (see `eden/LICENSE.txt`). Inspired by the
  [Eden emulator project](https://git.eden-emu.dev/eden-emu/eden); **not
  affiliated with or endorsed by** Eden, Nintendo, or Microsoft.
- Reused open-source components (e.g. libXenon) retain their own licenses; see
  the relevant files and folders.
- Not affiliated with the Super Mario 64 PC port or any other project.
