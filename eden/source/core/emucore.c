/* emucore.c: Eden360 bridge over the emucore modules (see emucore.h). */
#include "emucore.h"

#include <stdio.h>
#include <string.h>

#include "a64/a64.h"
#include "a64/a64_mem.h"
#include "hle/hle.h"
#include "loader/pfs0.h"
#include "loader/nso.h"
#include "loader/loader.h"

#include "a64_test_entry.h"

/* The whole guest address space lives in BSS (the frontend runs from a flat
 * static ELF; there is no host heap to speak of on libXenon). */
__attribute__((section(".guestram")))
static uint8_t g_ram[EMU_GUEST_RAM_SIZE];
static A64GuestMem g_mem;
static A64             g_cpu;
static HleState        g_hle;
static LoadedImage     g_img;
static int             g_inited;
static int             g_loaded;

int EmuCore_Init(void) {
    if (g_inited) return 1;
    A64GuestMem_Init(&g_mem, g_ram, sizeof(g_ram), 0);
    Hle_Init(&g_hle);
    A64GuestMem_Attach(&g_mem, &g_cpu);
    A64_SetSvcHandler(&g_cpu, Hle_HandleSvc, &g_hle);
    g_loaded = 0;
    g_inited = 1;
    return 1;
}

int EmuCore_Active(void) { return g_inited; }

const char *EmuCore_BackendName(void) {
    return "AArch64 interpreter (emucore/a64, partial)";
}

const char *EmuCore_StatusLine(void) {
    static char line[160];
    snprintf(line, sizeof(line),
             "AArch64 interpreter wired; guest RAM %u MB; "
             "NCAs need prod.keys (a retail NSP is listed, not booted)",
             (unsigned)(EMU_GUEST_RAM_SIZE / (1024u * 1024u)));
    return line;
}

int EmuCore_SelfTest(void) {
    return eden_a64_selftest();
}

const char *EmuCore_SelfTestReport(void) {
    return eden_a64_selftest_report();
}

int EmuCore_LoadNso(const void *data, unsigned long len) {
    Nso n;
    LoadResult lr;

    if (!g_inited) EmuCore_Init();
    g_loaded = 0;

    if (Nso_Parse(&n, data, (uint64_t)len) != NSO_OK)
        return 0; /* status reported by caller's reason string */
    lr = Nso_LoadIntoGuest(&n, data, (uint64_t)len, &g_mem, &g_img);
    if (lr != LOAD_OK) return 0;

    A64_Reset(&g_cpu, g_img.entry, 1);
    g_loaded = 1;
    return 1;
}

int EmuCore_ProbePfs0(const void *hdr, unsigned long hdr_len,
                      unsigned long long file_size, const char **reason) {
    Pfs0 p;
    Pfs0Result r = Pfs0_ParseEx(&p, hdr, (uint64_t)hdr_len, (uint64_t)file_size);
    if (r != PFS0_OK) {
        if (reason) *reason = Pfs0_ResultName(r);
        return -1;
    }
    if (reason) *reason = Pfs0_ResultName(PFS0_OK);
    return (int)p.count;
}

int EmuCore_RunSlice(unsigned long max_insns) {
    A64Halt h;
    if (!g_loaded) return 0;
    h = A64_Run(&g_cpu, (uint64_t)max_insns);
    if (h == A64_OK) return 0; /* budget exhausted; still runnable */
    return 1;
}

const char *EmuCore_HaltName(void) {
    return A64_HaltName(g_cpu.halt);
}

uint64_t EmuCore_PC(void)         { return g_cpu.pc; }
uint64_t EmuCore_Entry(void)      { return g_img.entry; }
uint64_t EmuCore_InstrCount(void) { return g_cpu.icount; }
