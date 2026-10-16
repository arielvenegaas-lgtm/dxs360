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

#ifdef __cplusplus
}
#endif
