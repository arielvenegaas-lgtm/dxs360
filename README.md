# Aurora Bricks

A 2D Breakout-style game for the **Xbox 360**. This repository can build it
three ways — two of them entirely with **open-source toolchains** that need no
Microsoft XDK and run in **GitHub Actions**:

| Path | Toolchain | Output | Playable? | Launches from |
|---|---|---|---|---|
| **A** | [libXenon](https://github.com/Free60Project/libxenon) | `AuroraBricks.elf` | **Yes** (graphics + input) | Aurora File Manager / XeLL |
| **B** | [FreeChainXenon](https://github.com/FreeChainXenon) | `default.xex` (genuine XEX2) | Bring-up only | Aurora File Manager |
| **C** | RXDK-360 + licensed XDK | `default.xex` (genuine XEX2) | Yes | Aurora / FSD / XeXMenu |

The goal is a genuine Xbox 360 title that launches on RGH/JTAG hardware. The
open-source world can now do **either** graphics (libXenon, but ELF) **or** a
real `.xex` (FreeChainXenon, but no graphics yet) — not both at once. So this
repo ships both so you can pick.

---

## Quick start (no compiler needed)

1. Open this repo's **Actions** tab and wait for a green run of
   **`build-libxenon (playable ELF)`** (Path A).
2. Download the **`AuroraBricks-libxenon`** artifact and unzip it.
3. Copy the `AuroraBricks` folder to your console (USB / HDD / FTP).
4. In **Aurora**, open the **File Manager**, browse to
   `AuroraBricks/AuroraBricks.elf`, and press **A**.

That elf is the **playable** build. Path B (`build-fcx`) additionally produces
a genuine `.xex`; see below for why it can't draw the game yet.

---

## Controls

| Input | Action |
|---|---|
| Left stick / D-pad | Move paddle |
| **A** | Launch ball / next level / restart after game over |
| **Start** | Pause / resume |

With no controller connected the game plays itself (autopilot) so you can
confirm the build renders.

---

## Path A — libXenon (playable ELF)

[libXenon](https://github.com/Free60Project/libxenon) is the mature open-source
bare-metal 360 SDK: real USB input, a Xenos display, and a compiler
(`xenon-gcc`). It emits a **PPC ELF** which Aurora's File Manager (ELF support
since 0.4b) and XeLL can load directly.

- Sources: `xenon/Makefile`, `xenon/source/{main,gfx,input}.cpp`.
- Reuses the API-agnostic game logic in `src/` (`game.cpp`, `game.h`, `font.h`,
  and the shared `gfx.h`/`input.h` interfaces).
- Renders straight into the GPU **scan-out framebuffer**. libXenon's console
  driver exposes that surface as CPU-writable and micro-tiled; `xenon/source/gfx.cpp`
  reproduces the same tiling math (`console_pset32`) to blit a linear
  `0xAARRGGBB` shadow framebuffer each frame, then flushes the data cache.
- Input maps libXenon's `controller_data_s` onto the project's `PadState`.

Build it yourself (Docker, as in CI):

```sh
docker run --rm -v "$PWD":/app -w /app free60/libxenon:latest \
  bash -lc 'make -C xenon'
# -> xenon/aurora_bricks.elf32
```

Launch: rename/copy to `AuroraBricks.elf` and run it from Aurora's File Manager.

---

## Path B — FreeChainXenon (genuine `.xex` bring-up)

[FreeChainXenon](https://github.com/FreeChainXenon) is the only free/libre
toolchain that emits a **real `XEX2`** (via `SynthXEX`). It builds binutils,
gcc, newlib and SynthXEX from source. It is very early: **no D3D9, no input, no
graphics layer**, so the game cannot be rendered with it yet.

The `build-fcx` workflow therefore builds a **bring-up** title:
`fcx/bringup.c` boots under the 360 OS, resolves a kernel export by ordinal
(`VdDisplayFatalError`), and calls it — the on-screen code is the proof that a
genuine XEX built here will actually execute on the console. The ELF is embedded
inside the PE as a `.elf` section, so the XEX is self-contained.

- CI (`build-fcx`): clones the GitHub mirrors of the FreeChainXenon repos,
  patches the toolchain's hard-coded `/opt/local` prefixes, drops newlib and the
  second GCC pass (unused, since `fcx/build.sh` links `-nostdlib`), pins
  `SynthXEX` to `0.0.5` (0.0.2 has a stack-VLA overflow in its SHA-1 pass that
  segfaults *after* writing the XEX), builds the toolchain (cached in Actions),
  then runs `fcx/build.sh`.
- `fcx/crt0.s` is vendored from `FCX-Test-2` (MPL-2.0). `fcx/bringup.c` and
  `fcx/build.sh` are this project's.

> This artifact is **not** the game. It exists to prove the open `.xex` path.

---

## Path C — RXDK-360 + licensed XDK (Windows)

The original, fully graphic title path: D3D9 + XInput through the proprietary
Microsoft XDK, driven by
[RXDK-360](https://github.com/Team-Resurgent/RXDK360) in Visual Studio.

1. Windows 10/11 + Visual Studio 2022, a full licensed 360 XDK, and RXDK-360
   (which adds the `Xbox 360` MSBuild platform).
2. Open `AuroraBricks.sln`, pick **Release** / **Xbox 360**, Build.
3. Emits `AuroraBricks.xex`. Dev-sign and rename for Aurora:

   ```text
   XexTool.exe -m d -o default.xex AuroraBricks.xex
   ```

Nothing here redistributes XDK content — it uses yours.

### How the D3D9 rendering works

Follows the 2D D3D9 path proven on retail hardware (from
[Stu1dd3m0n9/xbox-360-port-enablers](https://github.com/Stu1dd3m0n9/xbox-360-port-enablers),
MIT):

- **No runtime shader compilation.** `D3DXCompileShader` hangs the console, so
  blit shaders are precompiled into DWORD arrays (`src/shaders/blit_vs.h`,
  `blit_ps.h`).
- **The backbuffer cannot be locked.** Draw into a CPU buffer (`Gfx_Lock`) and
  upload it.
- **Textures are tiled.** Upload with `XGTileTextureLevel` into an `X8R8G8B8`
  texture, then blit with point sampling.
- **`main()` never returns** (returning powers the console off).

---

## Layout

```
.github/workflows/
  build-libxenon.yml   Path A: libXenon playable ELF (Docker, no XDK)
  build-fcx.yml        Path B: FreeChainXenon genuine XEX bring-up
ci/sanity.yml          repo-only integrity check (not a runnable workflow)
xenon/
  Makefile             libXenon build
  source/
    main.cpp           bare-metal entry + main loop
    gfx.cpp            scan-out framebuffer renderer (tiled blit)
    input.cpp          libXenon controller -> PadState
fcx/
  build.sh             FreeChainXenon build script
  crt0.s               vendored FCX startup (MPL-2.0)
  bringup.c            kernel-export bring-up program
src/
  main.cpp             Path C entry point and main loop (D3D9)
  gfx.h/.cpp           D3D9 device + tiled framebuffer upload/blit + primitives
  input.h/.cpp         XInput polling (local ABI) with edge detection
  game.h/.cpp          Breakout logic + HUD/overlay drawing (shared by all paths)
  font.h               5x7 bitmap font (no external assets)
  shaders/             blit_vs.hlsl / blit_ps.hlsl + precompiled blit_vs.h / blit_ps.h
AuroraBricks.sln / .vcxproj   Path C Visual Studio project (platform: Xbox 360)
```

The shared interface is `gfx.h` + `input.h` + `game.h`. `game.cpp` only ever
touches those, which is why the same game logic runs on libXenon, FreeChainXenon
and D3D9 unchanged.

---

## Toolchain reality (why all three paths exist)

- **FreeChainXenon** emits a genuine `XEX2` and is fully open, but its runtime
  is minimal — no graphics or input APIs. Good for a real `.xex`, not a game.
- **libXenon** has graphics + input and is open, but emits **ELF**, run from
  Aurora's File Manager or XeLL (not the coverflow title path).
- **RXDK-360 + XDK** gives a full graphics-capable title `.xex`, but the
  compiler/headers/D3D9 libs are proprietary and non-redistributable, and it is
  Windows/Visual Studio only.

Paths A and B **can** be built in GitHub Actions / on Linux; Path C cannot.

---

## Status and caveats

- Paths A and B are built by CI on every push (see the badge/actions). The
  libXenon ELF is verified to be `elf32-powerpc`; the FCX output is verified to
  be genuine `XEX2` with an embedded ELF.
- None of the paths has been run on physical hardware by the author. Treat the
  first launch as bring-up.
- Path C source is written against the documented XDK/D3D9 API and
  hardware-verified patterns but has not been compiled (no Windows/XDK here).
- No audio.

## License / attribution

- Original game code: your project, use freely.
- D3D9 blit technique + precompiled blit shader bytecode adapted from
  [Stu1dd3m0n9/xbox-360-port-enablers](https://github.com/Stu1dd3m0n9/xbox-360-port-enablers)
  (MIT).
- libXenon scan-out/tiling technique adapted from libXenon's `console.c`
  (BSD-3-Clause, Georg Lukas).
- `fcx/crt0.s` from FreeChainXenon `FCX-Test-2` (MPL-2.0).
- Microsoft XDK content is Microsoft's and is not included.
