# Aurora Bricks

A 2D Breakout-style game for the **Xbox 360**, built as a real **title-mode
`.xex`** so it launches directly from **Aurora** (or FSD/XeXMenu) on RGH/JTAG
consoles — no XeLL, no `.elf`.

- **Target:** Xbox 360, retail-runnable unsigned XEX (dev-signed with `XexTool`).
- **Toolchain:** [RXDK-360](https://github.com/Team-Resurgent/RXDK360) (Visual
  Studio 2022/2026 platform) + your own licensed **Xbox 360 XDK**.
- **Graphics:** D3D9. The game renders into a CPU framebuffer, which is
  tile-uploaded to a texture and blitted fullscreen with a precompiled shader.
- **Input:** XInput controllers (with an autopilot demo when no pad is present).

> **Read this first.** This project compiles to a genuine Xbox 360 title XEX.
> Producing a graphics-capable 360 title requires the **proprietary Microsoft
> Xbox 360 XDK** (compiler, headers, and the D3D9/XInput import libraries).
> RXDK-360 is the open-source Visual Studio integration that drives that XDK.
> There is **no fully open-source toolchain** that both emits a title XEX and
> exposes D3D9 today (OpenXeChain emits XEX but has no graphics layer; libXenon
> has graphics but only emits ELF). See "Toolchain reality" below.

---

## Controls

| Input | Action |
|---|---|
| Left stick / D-pad | Move paddle |
| **A** | Launch ball / next level / restart after game over |
| **Start** | Pause / resume |

If no controller is connected the game plays itself (autopilot) so you can
confirm the build renders.

---

## Requirements

1. **Windows 10/11** with **Visual Studio 2022** (or "18"). RXDK-360 is
   Windows-only; the XDK content pipeline is Windows-only.
2. A **full install of the licensed Xbox 360 XDK** (not a minimum install).
3. **[RXDK-360](https://github.com/Team-Resurgent/RXDK360)** installed. It adds
   the `Xbox 360` MSBuild platform and wires the XDK toolchain/headers into
   clang. Point its installer at your XDK setup.

Nothing in this repo redistributes Microsoft XDK content — it uses yours.

---

## Build

1. Open `AuroraBricks.sln` in Visual Studio 2022.
2. Select configuration **Release** (or **Release_LTCG**) and platform
   **`Xbox 360`**.
3. **Build → Build Solution**.

The build emits `AuroraBricks.xex` under the configuration output folder
(e.g. `Release\`). The XDK *deploy* step may report `X1001 Could not connect`
when no devkit is attached — that is deploy, not build, and the `.xex` is still
produced.

### Dev-sign and name it `default.xex`

RGH/JTAG and devkits run dev-signed XEXs. Sign and rename for Aurora:

```text
XexTool.exe -m d -o default.xex AuroraBricks.xex
```

`XexTool.exe` ships with RXDK-360 (and from
[Team-Resurgent/XexTool](https://github.com/Team-Resurgent/XexTool)).

---

## Deploy to the console

1. Copy `default.xex` to a folder on the console, e.g.
   `Hdd1:\Games\AuroraBricks\default.xex` (via FTP, `xbcp`, or a USB drive).
2. In **Aurora**, add that folder or launch the file from the File Manager.
   Aurora runs `default.xex` in title mode.

---

## How the rendering works

This follows the only D3D9 2D path proven on retail hardware (from
[Stu1dd3m0n9/xbox-360-port-enablers](https://github.com/Stu1dd3m0n9/xbox-360-port-enablers),
MIT). It encodes several Xbox 360 hardware constraints:

- **No runtime shader compilation.** `D3DXCompileShader` hangs the console, so
  the blit vertex/pixel shaders are precompiled into DWORD arrays
  (`src/shaders/blit_vs.h`, `blit_ps.h`). To change them, recompile on the PC:
  `fxc /nologo /T vs_3_0 /E main /Fh blit_vs.h /Vn g_blit_vs blit_vs.hlsl`
  (and the `ps_3_0` equivalent).
- **The backbuffer cannot be locked.** The game draws into its own CPU buffer
  (`Gfx_Lock`) and uploads it. Never call `LockRect` on the backbuffer.
- **Textures are tiled.** The CPU buffer is uploaded with `XGTileTextureLevel`
  (not a linear `memcpy`) into an `X8R8G8B8` texture, then blitted with
  point sampling and a half-texel inset.
- **`main()` never returns.** Returning calls `HalReturnToFirmware(0)` and
  powers the console off, so the loop is intentionally infinite.

Rendering is a software framebuffer, which is completely adequate for a 2D
brick-breaker and keeps the GPU path minimal and predictable.

---

## Layout

```
AuroraBricks.sln / .vcxproj   Visual Studio project (platform: Xbox 360)
src/
  main.cpp      entry point and main loop
  gfx.h/.cpp    D3D9 device + tiled framebuffer upload/blit + primitives
  input.h/.cpp  XInput polling (local ABI) with edge detection
  game.h/.cpp   Breakout logic + HUD/overlay drawing
  font.h        5x7 bitmap font (no external assets)
  shaders/      blit_vs.hlsl / blit_ps.hlsl + precompiled blit_vs.h / blit_ps.h
```

---

## Toolchain reality (why the build is Windows-only)

Verified while building this project:

- **OpenXeChain** (`SynthXEX` + LLVM fork) is the only *fully open* 360
  toolchain that emits XEX2, but its runtime library (`xecorelib`) is only
  `xam`/`xboxkrnl`/`hv` stubs — **no D3D9, no graphics**. It is also early and
  unverified on hardware.
- **RXDK-360** is the practical path to a real, graphics-capable title XEX, but
  it consumes the **licensed Microsoft XDK** for the compiler/headers/libs
  (D3D9, XInput). It is Windows + Visual Studio only.
- **libXenon** has graphics and is open, but emits **ELF** (run via XeLL /
  Aurora's File Manager), not a title `default.xex`.

Consequently this repo **cannot** be built in GitHub Actions / on Linux: the
XDK is proprietary and not redistributable. `ci/sanity.yml` is a repo-only
integrity check; move it to `.github/workflows/` to enable it as a real workflow.

---

## Status and caveats

- The source is written against the documented XDK/D3D9 API and the
  hardware-verified patterns above, but it has **not been compiled or run on a
  console by the author** (no Windows/XDK available). Treat the first build as
  a bring-up: check `DbgPrint` output over the debug channel.
- `Gfx_Lock` locks a `D3DPOOL_DEFAULT` texture. This mirrors the verified
  enabler code; if your XDK/driver rejects it, switch the pool to
  `D3DPOOL_MANAGED` in `src/gfx.cpp`.
- No audio. Add XAudio2 if you want it (the enabler has a proven push model).

## License / attribution

Original game code: your project, use freely. The D3D9 blit technique and the
precompiled blit shader bytecode are adapted from
[Stu1dd3m0n9/xbox-360-port-enablers](https://github.com/Stu1dd3m0n9/xbox-360-port-enablers)
(MIT). Xbox 360 XDK content is Microsoft's and is not included.
