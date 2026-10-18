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

The current milestone is the `nvdrv` (`INvDrvServices`) service HLE. The
retail title reaches `nvdrv`, and the emulator now handles the *non-GPU* part
of the driver's startup truthfully:

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
- **`Ioctl`** (cmd 1) is **not** implemented. The first ioctl is reported
  verbatim and the guest halts cleanly at that point.

Observed stop for the retail title (Cave Story+ NSP, `main` NSO):

```
ipc h=0x2a "nvdrv" type=5 cmd=3 sm=0 handled=1          # Initialize
ipc h=0x2a "nvdrv" type=4 cmd=3 sm=0 handled=1          # QueryPointerBufferSize (control)
ipc h=0x2a "nvdrv" type=5 cmd=4 sm=0 handled=1          # QueryEvent
ipc h=0x2a "nvdrv" type=4 cmd=8 sm=0 handled=1          # SetAruid
ipc h=0x2a "nvdrv" type=4 cmd=0 sm=0 handled=1          # Open("/dev/nvhost-ctrl") -> fd 1
ipc h=0x2a "nvdrv" type=4 cmd=1 sm=0 handled=0 \
    nvdrv Ioctl fd=1 dev="/dev/nvhost-ctrl" iocode=0xc183001b \
    sstat{0xbffeb98,0x183} sbuf{0x0,0x0} rbuf{0x0,0x0}: GPU ioctl unimplemented

session(x0=0x2a)=nvdrv   last_svc=0x21 (SendSyncRequestWithUserBuffer)
```

`0xc183001b` is an `nvhost-ctrl` ioctl: type `0x1b`, direction read|write,
387 (`0x183`) bytes of in/out argument carried in a **send static** at
`0xbffeb98`. The emulator decodes and reports the descriptor faithfully and
halts — it does not invent a response.

## Remaining work for the NVIDIA stack

To progress past the stop above, the following must be implemented for real
(no stubs that return success):

1. **`/dev/nvhost-ctrl` ioctls** — at minimum `NVHOST_IOCTL_CTRL_GET_SYNCPOINT`
   and the syncpoint wait/read family, backed by a real syncpoint table and a
   wait queue integrated with `WaitSynchronizationN`.
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
7. **Additional Horizon services** the title will request beyond `nvdrv`
   (filesystem/`fsp-srv`, `hid`, `am`, `vi`, `audren`, ...), each implemented
   truthfully or left as an honest stop.

## Tests and CI

Host tests for this component (run in `.github/workflows/build-eden.yml`):

- `eden/tests/test_a64_mem.c` — interpreter, guest memory, sparse heap.
- `eden/tests/test_hle_svc.c` — SVC dispatch, unknown logging, no fake success.
- `eden/tests/test_kernel.c` — QueryMemory, ticks, debug log, heap, GetInfo.
- `eden/tests/test_nvdrv.c` — HIPC descriptors and nvdrv Open/Close/errors.
- `eden/tests/test_pfs0_nso.c`, `test_mod0.c`, `test_layout.c`,
  `test_nca.c`, `test_emucore.c`, `nso_report.c` — loaders and bridge.

These are synthetic and host-only. The qemu-ppc (big-endian) interpreter run,
the libXenon/Docker console build, and anything requiring real USB/HDD
hardware cannot be exercised in a plain Linux environment and are **not**
verified by these tests. Nothing here has been validated on real console
hardware for the GPU path, because no GPU path exists yet.
