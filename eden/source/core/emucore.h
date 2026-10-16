/* emucore.h: the Eden360 emulation-core bridge over the emucore modules.
 *
 * This is the first real wiring of the portable AArch64 core into the frontend.
 * It is intentionally honest about what it can do: it provisions guest RAM,
 * brings up the interpreter, the HLE bridge and the PFS0/NSO loaders, runs a
 * deterministic self-test, and can load a standalone NSO and execute a bounded
 * slice of it. It cannot decrypt NCAs, so a retail NSP is parsed (container
 * listing) but not booted - and the API says so instead of pretending.
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
 * guest RAM size grow without disturbing the working USB layout. This is ample
 * for the small NSO modules the frontend runs today. */
#define EMU_GUEST_RAM_SIZE (8u * 1024u * 1024u)

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

/* Load a standalone NSO image (the format inside retail NCAs) from memory and
 * set the PC to its entry point. Returns 1 on success; on failure the reason is
 * available via EmuCore_StatusLine(). */
int EmuCore_LoadNso(const void *data, unsigned long len);

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

#ifdef __cplusplus
}
#endif
