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
#include "loader/mod0.h"
#include "loader/nca.h"
#include "loader/keys.h"

#include "a64_test_entry.h"

/* The whole guest address space lives in BSS (the frontend runs from a flat
 * static ELF; there is no host heap to speak of on libXenon). */
__attribute__((section(".guestram")))
static uint8_t g_ram[EMU_GUEST_RAM_SIZE];
/* Scratch for streaming an NSP's ExeFS section out of the container. Also in
 * .guestram (NOLOAD) so it never inflates low .bss and never disturbs the USB
 * DMA layout. Sized for a retail ExeFS section (~15.1 MB) plus its header. */
#define EMU_NSP_SCRATCH_SIZE (16u * 1024u * 1024u)
__attribute__((section(".guestram")))
static uint8_t g_nsp_scratch[EMU_NSP_SCRATCH_SIZE];
static A64GuestMem g_mem;
static A64             g_cpu;
static HleState        g_hle;
static LoadedImage     g_img;
static Mod0Info        g_mod0;
static ProdKeys        g_keys;
static int             g_have_keys;
static char            g_last_error[128];
static int             g_inited;
static int             g_loaded;
static int             g_relocated;

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
    uint64_t stub_base, stub_cap;

    if (!g_inited) EmuCore_Init();
    g_loaded = 0;
    g_relocated = 0;
    memset(&g_mod0, 0, sizeof(g_mod0));
    if (Nso_Parse(&n, data, (uint64_t)len) != NSO_OK)
        return 0; /* status reported by caller's reason string */
    lr = Nso_LoadIntoGuest(&n, data, (uint64_t)len, &g_mem, &g_img);
    if (lr != LOAD_OK) return 0;

    /* Apply the module's own dynamic relocations. A retail NSO stores a MOD0
     * header in .text that points at the ELF .dynamic section; without applying
     * those relocations every GOT/function pointer still holds its link-time
     * value and the first indirect call faults. We do NOT skip this: a failure
     * to relocate leaves g_relocated == 0 and the caller reports it. */
    if (Mod0_Locate(&g_mem, g_img.image_base, &g_mod0)) {
        stub_base = (g_img.image_end + 0xf) & ~(uint64_t)0xf;
        stub_cap  = sizeof(g_ram) > stub_base ? sizeof(g_ram) - stub_base : 0;
        g_mod0.stub_base = stub_base;
        if (stub_base >= sizeof(g_ram) ||
            !Mod0_Relocate(&g_mem, &g_mod0, g_img.image_base, stub_base, stub_cap))
            return 0;
        g_relocated = 1;
    }

    A64_Reset(&g_cpu, g_img.entry, 1);
    g_loaded = 1;
    return 1;
}

int EmuCore_SetKeys(const void *data, unsigned long len) {
    if (!g_inited) EmuCore_Init();
    if (!data || len == 0) { g_have_keys = 0; return 0; }
    Keys_Init(&g_keys);
    g_have_keys = (Keys_Parse(&g_keys, data, (size_t)len) == KEYS_OK);
    return g_have_keys;
}

int EmuCore_LoadNsp(EmuCoreReadFn rd, void *ctx, unsigned long long nsp_size) {
    const uint8_t *nso = 0;
    uint64_t nso_len = 0;
    NcaResult r;
    int ok;

    g_last_error[0] = 0;
    if (!g_inited) EmuCore_Init();
    if (!rd) { snprintf(g_last_error, sizeof(g_last_error), "no reader"); return 0; }
    if (!g_have_keys) {
        snprintf(g_last_error, sizeof(g_last_error),
                 "prod.keys not loaded (header_key/titlekek required)");
        return 0;
    }

    r = Nsp_ExtractMainStream(&g_keys, (NcaReadFn)rd, ctx, (uint64_t)nsp_size,
                              g_nsp_scratch, sizeof(g_nsp_scratch), &nso, &nso_len);
    if (r != NCA_OK) {
        snprintf(g_last_error, sizeof(g_last_error), "NSP extract: %s", Nca_ResultName(r));
        return 0;
    }
    ok = EmuCore_LoadNso(nso, (unsigned long)nso_len);
    if (!ok && g_last_error[0] == 0)
        snprintf(g_last_error, sizeof(g_last_error), "NSO load/relocate failed");
    return ok;
}

const char *EmuCore_LastError(void) { return g_last_error; }

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

const char *EmuCore_RelocReport(void) {
    static char line[224];
    if (!g_loaded) return "no module loaded";
    snprintf(line, sizeof(line),
             "entry=%#llx image_end=%#llx relocs=%llu (relative=%llu, jmprel=%llu) "
             "syms=%llu imports=%d unresolved=%llu",
             (unsigned long long)g_img.entry,
             (unsigned long long)g_img.image_end,
             (unsigned long long)g_mod0.rel_total,
             (unsigned long long)g_mod0.rel_relative,
             (unsigned long long)g_mod0.rel_jmprel,
             (unsigned long long)g_mod0.sym_total,
             g_mod0.import_count,
             (unsigned long long)g_mod0.sym_unresolved);
    return line;
}

const char *EmuCore_LastImportName(void) {
    if (!g_relocated) return 0;
    if (g_cpu.halt != A64_HALT_HLT) return 0;
    /* The HLT stub is 4 bytes and the PC has already advanced past it. */
    return Mod0_ImportNameFor(&g_mod0, g_cpu.pc - 4);
}
