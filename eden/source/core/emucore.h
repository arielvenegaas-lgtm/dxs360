/* emucore.h: the Eden360 emulation-core bridge over the emucore modules.
 *
 * This is the first real wiring of the portable AArch64 core into the frontend.
 * It is intentionally honest about what it can do: it provisions guest RAM,
 * brings up the interpreter, the HLE bridge and the PFS0/NSO/NCA loaders, runs a
 * deterministic self-test, loads a standalone NSO, and -- given a prod.keys file
 * -- streams a retail NSP, decrypts its program NCA + ExeFS, and maps every
 * module NSO at its kernel-assigned base in NsoIndex order (rtld first), then
 * enters the process at rtld. Dynamic linking is left entirely to the guest
 * rtld, exactly as on hardware; the host loader never resolves or fakes an
 * import. It emulates a small, honest subset of Horizon kernel calls
 * (QueryMemory, GetSystemTick, OutputDebugString, SetHeapSize, GetProcessId),
 * so rtld gets past the memory discovery it performs at startup; it still stops
 * when it needs a call that is not implemented, and the API reports exactly
 * where instead of pretending otherwise.
 *
 * Pure C (no libXenon headers) so the same code is exercised by the host test,
 * the big-endian PPC test and the bare-metal build.
 */
#pragma once

#include <stdint.h>

#include "hle/ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Guest RAM reserved for loaded modules. The real Xenon target has 512 MB of
 * shared RAM, so this is a modest, fixed reservation rather than a guess at the
 * Switch's 4 GB address space. Modules that link outside it fail to load with a
 * clear reason rather than silently corrupting memory. */
/* This array is placed in its own ".guestram" output section by eden/app.lds,
 * high above the heap, so it does NOT sit in low .bss. libXenon's EHCI USB
 * driver keeps its DMA structures (usb_os.c heapspace, plus malloc'd transfer
 * buffers) low in RAM, and the 360's EHCI cannot reach the whole 512 MB window,
 * so a large low .bss pushes those structures out of reach and USB mass storage
 * stops enumerating (64 MB -> ~78 MB image broke USB outright; 16 MB -> ~31 MB
 * still lost the devices on real hardware). Keeping it out of .bss lets the
 * guest RAM size grow without disturbing the working USB layout.
 *
 * A retail game's main NSO links to ~0x11bd000 (~17.7 MB: text+ro+data+bss),
 * so 8 MB was too small to even map it. 64 MB clears that with headroom for
 * guest heap/stack; it remains far inside the .guestram window (0x83000000 up
 * to 0x9E000000 in eden/app.lds). */
#define EMU_GUEST_RAM_SIZE (64u * 1024u * 1024u)

/* The process code region: every ExeFS module is mapped contiguously starting
 * here, and the process enters at the first module's base (rtld). Horizon's
 * (deprecated) 64-bit address space places a process's code at 0x8000000, and
 * this is exactly where rtld's module-discovery loop begins probing -- it walks
 * upward from a hard-coded 0x8000000 (the base of `LocateSpaceForMapDeprecated`,
 * bounded by 0x78000000 / a type-0x10 terminator) and treats every
 * CodeStatic + R|X region as a loadable module. Mapping the modules anywhere
 * else makes rtld discover nothing and fail its first symbol lookup. */
#define EMU_CODE_BASE 0x8000000ULL

/* A retail title asks the kernel for a multi-gigabyte heap (Cave Story+ computes
 * 0xC0000000 - image_base and reserves ~3 GB). A flat buffer cannot represent
 * that, so the process's address space is decoupled from the flat reservation:
 * the low 64 MB is real, flat RAM (modules + stack + TLS) and everything above
 * it is a sparse, demand-paged heap window. The window is fully addressable --
 * every heap access is bounds-checked and either resolves to a real frame or
 * faults honestly -- but only pages actually touched consume RAM, bounded by a
 * fixed frame budget because the Xenon target has 512 MB of shared RAM. */
#define EMU_VA_END 0x100000000ULL              /* process address-space top   */
#define EMU_HEAP_VA_BASE (EMU_CODE_BASE + EMU_GUEST_RAM_SIZE)  /* = 0xC000000  */
#define EMU_HEAP_VA_END  EMU_VA_END
#define EMU_HEAP_PAGE_SIZE 0x1000u             /* heap page / frame size      */
/* Real RAM the demand-paged heap may back before an access fails with a clear
 * out-of-memory fault. Kept inside the .guestram window alongside RAM/scratch. */
#define EMU_HEAP_FRAME_BUDGET (128u * 1024u * 1024u)
#define EMU_HEAP_PAGES ((EMU_HEAP_VA_END - EMU_HEAP_VA_BASE) / EMU_HEAP_PAGE_SIZE)

/* Initialise guest RAM, the AArch64 interpreter, the HLE bridge and install the
 * SVC handler. Idempotent. Returns 1 on success. */
int EmuCore_Init(void);

/* 1 once EmuCore_Init() has run. */
int EmuCore_Active(void);

/* Backend identity for the About/System screens. */
const char *EmuCore_BackendName(void);

/* One-line truthful status (what is up, what is missing). */
const char *EmuCore_StatusLine(void);

/* Run the fixed deterministic AArch64 program through the interpreter. Returns
 * 0 if every architectural check passed, otherwise the failure count. */
int EmuCore_SelfTest(void);
const char *EmuCore_SelfTestReport(void);

/* Load a standalone NSO image (the format inside retail NCAs) from memory,
 * apply its MOD0 dynamic relocations, and set the PC to its entry point.
 * Returns 1 on success; on failure the reason is available via
 * EmuCore_StatusLine(). */
int EmuCore_LoadNso(const void *data, unsigned long len);

/* Provide the prod.keys text needed by the NCA content layer. Returns 1 if at
 * least one usable key was parsed. */
int EmuCore_SetKeys(const void *data, unsigned long len);

/* Random-access reader used to stream a large container. */
typedef long (*EmuCoreReadFn)(void *ctx, unsigned long long off,
                              void *buf, unsigned long len);

/* Load and run a retail NSP's program NSO: stream the container through `rd`,
 * decrypt the program NCA + ExeFS, relocate the extracted main NSO and set the
 * PC to its entry. Requires EmuCore_SetKeys() first. Returns 1 on success; the
 * exact failure otherwise via EmuCore_LastError(). */
int EmuCore_LoadNsp(EmuCoreReadFn rd, void *ctx, unsigned long long nsp_size);

/* Human-readable description of the last load attempt's failure ("" if none). */
const char *EmuCore_LastError(void);

/* Parse an NSP/PFS0 header region. `hdr` holds the first `hdr_len` bytes and
 * `file_size` is the real container size (used to validate entry ranges without
 * reading the whole file). Returns the file count, or -1 on error (reason via
 * *out_reason when non-NULL). */
int EmuCore_ProbePfs0(const void *hdr, unsigned long hdr_len,
                      unsigned long long file_size, const char **out_reason);

/* Run up to max_insns instructions. Returns 1 if execution stopped (halt or an
 * unimplemented encoding), 0 if it is still running after the budget. */
int EmuCore_RunSlice(unsigned long max_insns);

/* State of the last run. */
const char *EmuCore_HaltName(void);
uint64_t EmuCore_PC(void);
uint64_t EmuCore_Entry(void);
uint64_t EmuCore_InstrCount(void);

/* The address/size/write flag of the last memory fault (valid when
 * EmuCore_HaltName() reports a memory fault). Any pointer may be NULL. */
void EmuCore_FaultInfo(uint64_t *addr, uint64_t *size, int *write);

/* 1 if the last memory fault was the demand-paged heap running out of frames
 * (the guest touched more of its reservation than the fixed frame budget can
 * back), so the caller can report it as an out-of-memory event. */
int EmuCore_FaultIsOOM(void);

/* One-line report of the demand-paged heap: its guest base/size, bytes backed
 * by real frames, and the frame budget. Never NULL. */
const char *EmuCore_HeapReport(void);

/* One-line summary of the last load's MOD0 relocation work (entry, reloc
 * counts, import count). Never NULL. */
const char *EmuCore_RelocReport(void);

/* Deprecated: the host loader no longer generates import stubs (rtld performs
 * all dynamic linking at runtime), so this always returns NULL. Kept for ABI
 * stability with the frontend. */
const char *EmuCore_LastImportName(void);

/* Name of the loaded module whose [base, base+size) contains `addr`, or NULL.
 * Lets the frontend report exactly which module a stop/fault happened in. */
const char *EmuCore_ModuleAt(uint64_t addr);

/* Syscall number of the last SVC the HLE layer could not handle (0 if none).
 * Non-zero after a stop reported as "svc (unhandled)": exactly which kernel
 * call the process needs next. */
uint64_t EmuCore_LastSvc(void);

/* Human-readable name of that syscall ("" / "?" when unknown). */
const char *EmuCore_LastSvcName(void);

/* Copy `len` bytes from guest address `addr` into `buf`. Returns the number of
 * bytes read (0 if the range is outside the mapped reservation). */
unsigned long EmuCore_ReadGuest(uint64_t addr, void *buf, unsigned long len);

/* Port name a connected session handle refers to, or NULL if unknown. */
const char *EmuCore_SessionName(uint64_t handle);

/* Copy X0..X7 as they were at the last SVC into `out` (8 entries). Shows the
 * exact arguments of the kernel call the guest needs next. */
void EmuCore_LastSvcArgs(unsigned long long out[8]);

/* Full register snapshot of the current stop: out[0..30]=x0..x30,
 * out[31]=sp, out[32]=pc (same value as EmuCore_PC). Any pointer may be NULL. */
void EmuCore_DumpRegs(unsigned long long out[33]);

/* Debug: PC of the first instruction in the last run that dropped sp to (near)
 * zero (0 if sp never collapsed). Pinpoints stack-corruption sources. */
uint64_t EmuCore_SpZeroPc(void);

/* One QueryMemory probe as seen by the guest, plus how rtld's module scan
 * treats the region it resolved to. */
typedef struct EmuQueryProbe {
    unsigned long long addr;   /* address the guest probed            */
    unsigned long long base;   /* region base the kernel answered     */
    unsigned long long size;   /* region size the kernel answered     */
    unsigned type;             /* MemoryType                          */
    unsigned perm;             /* permission bits (R=1 W=2 X=4)       */
    const char *verdict;       /* rtld accept/reject/stop reason      */
} EmuQueryProbe;

/* Copy up to `max` recent QueryMemory probes (oldest first) into `out` and
 * return the number written. Shows exactly how rtld discovers the module
 * layout and why it accepts, skips or stops on each region. */
int EmuCore_QueryTrace(EmuQueryProbe *out, int max);

/* One IPC request as sent by the guest, plus whether this layer answered it.
 * Shows the exact service conversation the SDK is driving. */
typedef struct EmuIpcTrace {
    unsigned long long handle;
    char     name[HLEK_SESSION_NAME];
    unsigned type;            /* HIPC message type                     */
    unsigned cmd;             /* CMIF method id / manager cmd          */
    int      is_sm;           /* target was the service manager        */
    int      handled;         /* 1 = response written                  */
    char     note[HLEIPC_NOTE];
} EmuIpcTrace;

/* Copy up to `max` recent IPC requests (oldest first) into `out`. */
int EmuCore_IpcTrace(EmuIpcTrace *out, int max);

#ifdef __cplusplus
}
#endif
