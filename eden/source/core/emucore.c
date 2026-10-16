/* emucore.c: Eden360 bridge over the emucore modules (see emucore.h). */
#include "emucore.h"

#include <stdio.h>
#include <string.h>

#include "a64/a64.h"
#include "a64/a64_mem.h"
#include "hle/hle.h"
#include "hle/kernel.h"
#include "loader/pfs0.h"
#include "loader/nso.h"
#include "loader/loader.h"
#include "loader/layout.h"
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
static HleKernel       g_kernel;
static LoadedImage     g_img;
static ProdKeys        g_keys;
static int             g_have_keys;
static char            g_last_error[160];
static int             g_inited;
static int             g_loaded;

/* Kernel-style placement of the ExeFS modules (rtld first, page-aligned,
 * contiguous) and a per-module record for honest reporting. */
static LayoutPlan      g_layout;
static int             g_module_count;
static uint64_t        g_entry;

typedef struct EmuModule {
    char     name[16];
    int      rank;
    uint64_t base;
    uint64_t size;
    int      has_mod0;
    uint64_t dynamic;
    uint64_t init;
} EmuModule;

static EmuModule       g_mods[LAYOUT_MAX_MODULES];

/* Top of guest RAM: the process stack grows down from here. The kernel gives
 * the main thread a stack above the mapped modules; the flat guest-RAM model
 * mirrors that by placing SP at the end of the reservation. */
static uint64_t GuestStackTop(void) {
    return (uint64_t)sizeof(g_ram) & ~(uint64_t)0xf;
}

int EmuCore_Init(void) {
    if (g_inited) return 1;
    A64GuestMem_Init(&g_mem, g_ram, sizeof(g_ram), 0);
    Hle_Init(&g_hle);
    HleKernel_Init(&g_kernel);
    HleKernel_SetSpanEnd(&g_kernel, sizeof(g_ram));
    HleKernel_Register(&g_hle, &g_kernel);
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
             "AArch64 interpreter wired; guest RAM %u MB; maps all ExeFS modules "
             "and enters rtld (dynamic linking runs in guest)",
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
    g_module_count = 0;
    memset(&g_layout, 0, sizeof(g_layout));
    if (Nso_Parse(&n, data, (uint64_t)len) != NSO_OK)
        return 0; /* status reported by caller's reason string */
    lr = Nso_LoadIntoGuest(&n, data, (uint64_t)len, &g_mem, 0, &g_img);
    if (lr != LOAD_OK) return 0;

    /* No host-side dynamic linking here. A retail module's imports live in
     * other modules (rtld/sdk) and are resolved by rtld at runtime, exactly as
     * on hardware. A standalone module without rtld is therefore not a complete
     * program; that is reported honestly rather than papered over with stubs. */
    g_entry = g_img.entry;
    A64_Reset(&g_cpu, g_entry, 1);
    g_cpu.sp = GuestStackTop();
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

/* Describe the mapped memory to the kernel-call layer: every module segment
 * (text/rodata/data/bss) with its protection, plus the process stack. This is
 * what rtld and the SDK observe through QueryMemory; it mirrors the kernel's
 * view of the process, not a host-side convenience map. */
static void BuildKernelRegions(void) {
    int m;
    uint32_t page = LAYOUT_PAGE;

    g_kernel.count = 0;
    HleKernel_SetSpanEnd(&g_kernel, sizeof(g_ram));

    for (m = 0; m < g_layout.count; ++m) {
        const LayoutModule *lm = &g_layout.mod[m];
        int s;
        for (s = 0; s < 4; ++s) {
            const LayoutSegment *sg = &lm->seg[s];
            uint32_t type, perm;
            if (sg->size == 0) continue;
            switch (s) {
                case LAYOUT_SEG_TEXT:
                    type = HLEK_MEM_CODE_STATIC;
                    perm = HLEK_PERM_R | HLEK_PERM_X;
                    break;
                case LAYOUT_SEG_RO:
                    type = HLEK_MEM_CODE_RO;
                    perm = HLEK_PERM_R;
                    break;
                default: /* data, bss */
                    type = HLEK_MEM_CODE_MUT;
                    perm = HLEK_PERM_R | HLEK_PERM_W;
                    break;
            }
            HleKernel_AddRegion(&g_kernel, sg->addr, sg->size, type, perm);
        }
    }

    /* The process stack sits above the modules, growing down from the top of
     * the guest reservation. */
    {
        uint64_t base = (g_layout.total_size + page - 1) & ~(uint64_t)(page - 1);
        uint64_t top  = (uint64_t)sizeof(g_ram);
        if (top > base)
            HleKernel_AddRegion(&g_kernel, base, top - base,
                                HLEK_MEM_STACK, HLEK_PERM_R | HLEK_PERM_W);
    }
}

/* Load every NSO module in a decrypted ExeFS PFS0 at its kernel-assigned base,
 * in NsoIndex order (rtld first). "main.npdm" and non-module members are
 * skipped. No import is resolved here: rtld performs all dynamic linking.
 * Returns 1 on success (g_layout/g_mods/g_entry filled), else 0 with
 * g_last_error set. */
static int LoadExefsModules(const uint8_t *exefs, uint64_t exefs_len) {
    Pfs0 pfs;
    LayoutInput in[LAYOUT_MAX_MODULES];
    uint32_t i;
    int n = 0, m;

    if (Pfs0_Parse(&pfs, exefs, exefs_len) != PFS0_OK) {
        snprintf(g_last_error, sizeof(g_last_error), "ExeFS is not a valid PFS0");
        return 0;
    }

    /* Gather the loadable modules with their geometry. */
    for (i = 0; i < pfs.count && n < LAYOUT_MAX_MODULES; ++i) {
        const char *nm = Pfs0_Name(&pfs, i);
        uint64_t sz;
        const uint8_t *d;
        Nso tmp;
        if (Layout_Rank(nm) < 0) continue;          /* e.g. main.npdm */
        d = Pfs0_Data(&pfs, i, &sz);
        if (!d) continue;
        if (Nso_Parse(&tmp, d, sz) != NSO_OK) {
            snprintf(g_last_error, sizeof(g_last_error), "module %s: bad NSO header", nm);
            return 0;
        }
        in[n].name = nm;
        in[n].image_size = Nso_ImageSize(&tmp);
        in[n].seg_loc[0] = tmp.seg[0].location; in[n].seg_size[0] = tmp.seg[0].size;
        in[n].seg_loc[1] = tmp.seg[1].location; in[n].seg_size[1] = tmp.seg[1].size;
        in[n].seg_loc[2] = tmp.seg[2].location; in[n].seg_size[2] = tmp.seg[2].size;
        in[n].bss_size  = tmp.seg[2].bss_size;
        ++n;
    }
    if (n == 0) {
        snprintf(g_last_error, sizeof(g_last_error), "ExeFS has no loadable modules");
        return 0;
    }

    if (!Layout_Plan(in, n, 0, &g_layout)) {
        snprintf(g_last_error, sizeof(g_last_error),
                 "module layout invalid (overlap/overflow/bad segment)");
        return 0;
    }

    g_module_count = 0;
    for (m = 0; m < g_layout.count; ++m) {
        const LayoutModule *lm = &g_layout.mod[m];
        int idx = Pfs0_Find(&pfs, lm->name);
        uint64_t sz;
        const uint8_t *d;
        Nso nn;
        LoadedImage img;
        LoadResult lr;
        Mod0Info mi;

        if (idx < 0) {
            snprintf(g_last_error, sizeof(g_last_error), "module %s vanished", lm->name);
            return 0;
        }
        d = Pfs0_Data(&pfs, (uint32_t)idx, &sz);
        if (!d || Nso_Parse(&nn, d, sz) != NSO_OK) {
            snprintf(g_last_error, sizeof(g_last_error), "module %s: bad NSO header", lm->name);
            return 0;
        }
        lr = Nso_LoadIntoGuest(&nn, d, sz, &g_mem, lm->base, &img);
        if (lr != LOAD_OK) {
            snprintf(g_last_error, sizeof(g_last_error), "module %s: map failed (%s)",
                     lm->name, Load_ResultName(lr));
            return 0;
        }

        /* Parse (do not apply) the MOD0/dynamic metadata so the layout can be
         * verified against the image and reported. */
        memset(&mi, 0, sizeof(mi));
        memset(&g_mods[g_module_count], 0, sizeof(g_mods[g_module_count]));
        {
            size_t nl = strlen(lm->name);
            if (nl >= sizeof(g_mods[g_module_count].name))
                nl = sizeof(g_mods[g_module_count].name) - 1;
            memcpy(g_mods[g_module_count].name, lm->name, nl);
        }
        g_mods[g_module_count].rank = lm->rank;
        g_mods[g_module_count].base = lm->base;
        g_mods[g_module_count].size = lm->size;
        if (Mod0_Locate(&g_mem, lm->base, &mi)) {
            g_mods[g_module_count].has_mod0 = 1;
            g_mods[g_module_count].dynamic = mi.dynamic;
            g_mods[g_module_count].init = mi.init;
        }
        g_module_count++;
    }

    g_entry = g_layout.entry;   /* == code_address, i.e. the rtld base */
    g_img.entry      = g_layout.entry;
    g_img.image_base = 0;
    g_img.image_end  = g_layout.total_size;
    g_img.mapped     = 0;

    BuildKernelRegions();
    return 1;
}

int EmuCore_LoadNsp(EmuCoreReadFn rd, void *ctx, unsigned long long nsp_size) {
    const uint8_t *exefs = 0;
    uint64_t exefs_len = 0;
    NcaResult r;

    g_last_error[0] = 0;
    g_loaded = 0;
    g_module_count = 0;
    if (!g_inited) EmuCore_Init();
    if (!rd) { snprintf(g_last_error, sizeof(g_last_error), "no reader"); return 0; }
    if (!g_have_keys) {
        snprintf(g_last_error, sizeof(g_last_error),
                 "prod.keys not loaded (header_key/titlekek required)");
        return 0;
    }

    /* Stream the container, decrypt the program NCA's ExeFS, and hand back the
     * whole decrypted ExeFS PFS0 (every module, not just "main"). */
    r = Nsp_ExtractExefsStream(&g_keys, (NcaReadFn)rd, ctx, (uint64_t)nsp_size,
                               g_nsp_scratch, sizeof(g_nsp_scratch), &exefs, &exefs_len);
    if (r != NCA_OK) {
        snprintf(g_last_error, sizeof(g_last_error), "NSP extract: %s", Nca_ResultName(r));
        return 0;
    }
    if (!LoadExefsModules(exefs, exefs_len)) return 0;

    /* Enter the process at rtld (the first module's base). The kernel starts the
     * main thread with X0=0 and a stack above the mapped modules; rtld then
     * locates, maps and relocates the remaining modules itself. */
    A64_Reset(&g_cpu, g_entry, 1);
    g_cpu.sp = GuestStackTop();
    g_cpu.x[0] = 0;
    g_cpu.x[1] = 0;
    g_loaded = 1;
    return 1;
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

uint64_t EmuCore_PC(void) {
    /* When the CPU halts on a bad encoding, g_cpu.pc has already been advanced
     * past the faulting word; report the faulting address instead. */
    if (g_cpu.halt == A64_HALT_UNIMPLEMENTED || g_cpu.halt == A64_HALT_UNDEFINED)
        return g_cpu.undefined_pc;
    return g_cpu.pc;
}
uint64_t EmuCore_Entry(void)      { return g_img.entry; }
uint64_t EmuCore_InstrCount(void) { return g_cpu.icount; }

uint64_t EmuCore_LastSvc(void) { return g_hle.last_unknown; }

const char *EmuCore_LastSvcName(void) {
    static char line[32];
    uint64_t svc = g_hle.last_unknown;
    const char *nm = HleKernel_SvcName(svc);
    if (nm[0] != '?') return nm;
    snprintf(line, sizeof(line), "syscall 0x%llx", (unsigned long long)svc);
    return line;
}

const char *EmuCore_RelocReport(void) {
    static char line[224];
    if (!g_loaded) return "no module loaded";
    if (g_module_count > 0) {
        snprintf(line, sizeof(line),
                 "modules=%d entry=rtld@%#llx span=%#llx (dynamic linking left to rtld)",
                 g_module_count,
                 (unsigned long long)g_layout.entry,
                 (unsigned long long)g_layout.total_size);
        return line;
    }
    snprintf(line, sizeof(line),
             "standalone module entry=%#llx image_end=%#llx (no host-side linking)",
             (unsigned long long)g_img.entry,
             (unsigned long long)g_img.image_end);
    return line;
}

/* Names the module containing `addr`, or NULL. Replaces the old import-stub
 * lookup: the host loader no longer creates stubs (rtld links at runtime), so
 * an HLT/brk is reported by module + offset instead of a fake import name. */
const char *EmuCore_ModuleAt(uint64_t addr) {
    int i;
    for (i = 0; i < g_module_count; ++i) {
        if (addr >= g_mods[i].base && addr < g_mods[i].base + g_mods[i].size)
            return g_mods[i].name;
    }
    return 0;
}

const char *EmuCore_LastImportName(void) {
    /* No host-side import resolution any more; a stop is reported by module. */
    return 0;
}
