# emucore

A small, honest ARM64 (AArch64) emulation core used by the Eden frontend to
investigate running real Switch titles on the Xbox 360. It is deliberately
split into independent, host-testable pieces:

```
emucore/
├── a64/       aarch64 interpreter + guest memory (flat + sparse demand heap)
├── hle/       Horizon kernel SVC dispatch and CMIF/HIPC service HLE
├── loader/    NSP/NCA/ExeFS/PFS0/NSO/MOD0/NPDM loaders, layout planner
└── crypto/    AES (XTS/CTR/ECB) + prod.keys parser
```

Nothing here fakes success. When the guest calls something we do not implement,
the core stops and reports the exact call (SVC number, or the service name,
command, and arguments of the unhandled IPC request) instead of returning a
plausible-looking result.

> **This does not boot or render a game.** There is no GPU, no nvhost/nvmap
> implementation, no audio, and no display. The emulator runs guest code up to
> the first genuinely unsupported operation and halts there with a faithful
> report.

## Verified milestone: nvdrv service opens honestly, then stops at the first GPU ioctl

The `nvdrv` (`INvDrvServices`) service HLE is handled up to the point where a
GPU is required. The retail title reaches `nvdrv`, and the emulator handles the
*non-GPU* part of the driver's startup truthfully:

- **HIPC descriptor parsing** (`hle/ipc.c`) decodes copy/move handles, send
  statics, send buffers, and receive buffers with bounds checks against the
  table the header declared, so a malformed or out-of-range descriptor is
  rejected rather than read out of bounds.
- **`Initialize`** (cmd 3) and **`SetAruid`** (cmd 8) are acknowledged with
  `Err = 0`; they carry no GPU semantics.
- **`Open`** (cmd 0) validates the device path and allocates an honest file
  descriptor from a fixed table for recognized `nvdrv` device names
  (`/dev/nvmap`, `/dev/nvhost-ctrl`, `/dev/nvhost-as-gpu`, ...). The SDK sends
  exactly `strlen(path)` bytes in a type-0x5 send buffer and does **not**
  NUL-terminate it, so the path reader reads a bounded number of bytes and
  terminates the string itself. Unknown devices return
  `NV_FILE_OPERATION_FAILED` (`0x30003`); malformed descriptors return
  `NV_BAD_PARAMETER` (`0x4`).
- **`Close`** (cmd 2) validates the handle and frees the slot; double-close and
  unknown handles return `NV_BAD_PARAMETER`.
- **`Ioctl`** (cmd 1) request numbers are decoded with the real NVIDIA
  encoding macros (`hle/nv_ioctl.h`, the Linux-derived primitive from libnx
  `nvidia/ioctl.h`), not guessed from numeric values. One command is
  implemented (see below); every other command is reported verbatim (decoded
  dir/type/nr/size, argument address/size, receive buffer) and the guest halts
  cleanly.

### `NVHOST_IOCTL_CTRL_GET_CONFIG` (`/dev/nvhost-ctrl`)

The first ioctl the retail title issues is `0xC183001B`. Decoded with the
macros it is `NV_IOC(inout, type=0x00, nr=0x1B, size=0x183)` —
**`NVHOST_IOCTL_CTRL_GET_CONFIG`**, a 387-byte inout argument
(switchbrew "NV services"):

```
struct {
  __in  char name[0x41];   // domain string, e.g. "nv"
  __in  char key[0x41];    // e.g. "NV_MEMORY_PROFILER"
  __out char value[0x101]; // configuration string
};
```

The title asks for `name="nv"`, `key="NV_MEMORY_PROFILER"` (traced from the
guest argument in a send static). switchbrew documents this command as
*"Not available in production mode"*, and yuzu/Ryujinx both return
`0x30006` for it there. Because a retail title runs on production hardware,
the verified answer is the error `0x30006`, **not** an invented
configuration value; the `value[]` region is left untouched. This is the only
ioctl implemented so far, and it fabricates nothing.

Observed flow for the retail title (Cave Story+ NSP, `main` NSO):

```
ipc h=0x2a "nvdrv" type=5 cmd=3 sm=0 handled=1          # Initialize
ipc h=0x2a "nvdrv" type=4 cmd=3 sm=0 handled=1          # QueryPointerBufferSize (control)
ipc h=0x2a "nvdrv" type=5 cmd=4 sm=0 handled=1          # QueryEvent
ipc h=0x2a "nvdrv" type=4 cmd=8 sm=0 handled=1          # SetAruid
ipc h=0x2a "nvdrv" type=4 cmd=0 sm=0 handled=1          # Open("/dev/nvhost-ctrl") -> fd 1
ipc h=0x2a "nvdrv" type=4 cmd=1 sm=0 handled=1          # Ioctl GET_CONFIG -> Err 0x30006
...
```

The `GET_CONFIG` answer is accepted and the guest continues through many other
services; the run no longer stops anywhere in `nvdrv`. Where the title halts now
is described by the next section.

## Verified milestone: display-service acquisition, stopping at OpenDisplay

`vi:m` is `nn::visrv::sf::IManagerRootService` and its command 2 is
`GetDisplayService` (switchbrew "Display services"). It takes a `u32
serviceType` and returns an `nn::visrv::sf::IApplicationDisplayService`:

- **`vi:m` cmd 2 `GetDisplayService`** (the title sends `serviceType = 1`, the
  system/manager service) hands back a session named `IApplicationDisplayService`.
- On that object the title then uses only the documented **get-session**
  commands, each of which returns a further session *by name* and asserts no
  display state at all:
  - cmd 102 `GetManagerDisplayService` -> `IManagerDisplayService`
  - cmd 101 `GetSystemDisplayService` -> `ISystemDisplayService`
  - cmd 100 `GetRelayService` -> `IHOSBinderDriver`
- The next call is cmd **1010 `OpenDisplay("Default")`**, traced straight out of
  the guest message (`params` is the 0x40-byte `DisplayName` block holding
  `"Default"`, exactly the string switchbrew documents for the default display).
  It is **not** implemented, so the run stops there and reports:

```
ipc h=0x2c "vi:m"  type=4 cmd=2  sm=0 handled=1
ipc h=0x2f "IApplicationDisplayService" type=4 cmd=102 sm=0 handled=1
ipc h=0x2f "IApplicationDisplayService" type=4 cmd=101 sm=0 handled=1
ipc h=0x2f "IApplicationDisplayService" type=4 cmd=100 sm=0 handled=1
ipc h=0x2f "IApplicationDisplayService" type=4 cmd=1010 sm=0 handled=0 \
    service "IApplicationDisplayService" Invoke2MethodOld cmd 1010 unimplemented

session(x0=0x2f)=IApplicationDisplayService   last_svc=0x21 (SendSyncRequestWithUserBuffer)
guest[0xbfff000]: SFCI cmd=1010 params="Default"
```

`OpenDisplay` is the first call that would have to materialize display state —
a `DisplayId`, a layer table, a compositor. The emulator does not model a
display yet, so it stops there instead of inventing one. No display, layer or
frame state is fabricated anywhere in the chain above.

## Remaining work for the display stack

To progress past `OpenDisplay` without faking anything:

1. **Display registry** — the documented display names (`"Default"`,
   `"External"`, `"Edid"`, `"Internal"`, `"Null"`) with their `DisplayInfo`
   (0x60 bytes: `Name[0x40]`, `HasLayerLimit`, `LayerCountMax`,
   `LayerWidthPixelCountMax`, `LayerHeightPixelCountMax`). The layout is
   documented; the retail `HasLayerLimit`/`LayerCountMax` values are **not**, so
   `ListDisplays` must wait until they are obtained rather than guessed.
2. **`OpenDisplay` / `CloseDisplay` / `GetDisplayResolution`** — signatures are
   documented, as is the fact that `"Default"` reports 1080p even handheld. The
   retail `DisplayId` numbering is not documented; the emulator would have to
   declare its own opaque ids and say so.
3. **`CreateStrayLayer` / `OpenLayer` / `CloseLayer`** — return a
   `NativeWindow` serialized as an Android `Parcel` (layout documented) carrying
   a `dispdrv` binder object, which needs `IHOSBinderDriver` first.
4. **`GetDisplayVsyncEvent`** — a real vsync source. Handing the guest an event
   that never fires would hang it in `WaitSynchronizationN`, so this must not be
   stubbed.
5. **`IManagerDisplayService` layer commands**, and everything past them, only
   once the nvmap/GM20B pipeline below actually exists.

## Remaining work for the NVIDIA stack

No GPU path exists yet, so nothing below can honestly return success. To
progress (and eventually to give the display stack above something to present
to), the following must be implemented for real:

1. **`/dev/nvhost-ctrl` ioctls** — `GET_CONFIG` is done (production error);
   still needed: `NVHOST_IOCTL_CTRL_GET_SYNCPOINT` and the syncpoint
   wait/read/incr family, backed by a real syncpoint table and a wait queue
   integrated with `WaitSynchronizationN`.
2. **`/dev/nvmap`** — `NVMAP_IOC_CREATE` / `..._ALLOC` / `..._FROM_ID` /
   `..._GET_ID` / `..._PINCACHE` and friend. Requires a real nvmap handle
   table over the guest's sparse heap (`A64GuestMem`), so mappings and
   allocations are accounted for, not faked.
3. **`/dev/nvhost-as-gpu`** — address-space (`NVGPU_AS_IOCTL_BIND_CHANNEL`,
   `..._ALLOC_SPACE`, `..._MAP_BUFFER`) plus the GM20B channel/GPFIFO submit
   path. Needs a real GPU address-space abstraction on top of nvmap.
4. **`/dev/nvhost-gpu`** — GM20B engine: channel setup, GPFIFO submission,
   syncpoint fences, and an actual command-processor / shader engine. This is
   the largest missing component and gates any rendering.
5. **`/dev/nvhost-vic`, `/dev/nvhost-nvdec`, `/dev/nvhost-msenc`** — the copy
   / decode / encode engines the title may open next.
6. **`nvdisp` / display and `nvdrv` `QueryEvent` results** — only once a real
   GPU/driver pipeline exists to service them.
7. **Additional Horizon services** the title will request (filesystem/`fsp-srv`,
   `hid`, `am`, `audren`, ...), each implemented truthfully or left as an honest
   stop. The `vi` root/display services are the one part already reached (see
   the display milestone above).

## Tests and CI

Host tests for this component (run in `.github/workflows/build-eden.yml`):

- `eden/tests/test_a64_mem.c` — interpreter, guest memory, sparse heap.
- `eden/tests/test_hle_svc.c` — SVC dispatch, unknown logging, no fake success.
- `eden/tests/test_kernel.c` — QueryMemory, ticks, debug log, heap, GetInfo.
- `eden/tests/test_nvdrv.c` — HIPC descriptors, nvdrv Open/Close/errors,
  `GET_CONFIG` decode + production result, honest stop for other ioctls.
- `eden/tests/test_vi.c` — `vi:m` `GetDisplayService` and the
  `IApplicationDisplayService` get-session commands hand out correctly named
  sessions; `ListDisplays`/`OpenDisplay` stay honest stops and are never
  reported as succeeding.
- `eden/tests/test_pfs0_nso.c`, `test_mod0.c`, `test_layout.c`,
  `test_nca.c`, `test_emucore.c`, `nso_report.c` — loaders and bridge.

These are synthetic and host-only. The qemu-ppc (big-endian) interpreter run,
the libXenon/Docker console build, and anything requiring real USB/HDD
hardware cannot be exercised in a plain Linux environment and are **not**
verified by these tests. Nothing here has been validated on real console
hardware for the GPU path, because no GPU path exists yet.
