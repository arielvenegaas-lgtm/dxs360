/* core.cpp: the Eden360 core interface, backed by the emucore/a64 bridge.
 *
 * This is no longer a pure stub: Core_Init() brings up guest RAM, the AArch64
 * interpreter, the HLE bridge and the PFS0/NSO loaders (emucore.c), and runs a
 * deterministic interpreter self-test. Core_CanBoot() is honest about the one
 * thing that can actually run today - a standalone NSO image - while a retail
 * NSP is parsed (container listing) but not booted, because NCA decryption
 * needs prod.keys and correct content. Nothing here fakes success.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "emucore.h"
#include "fs.h"

static int g_selftest_fails = -1; /* -1 = not run yet */

const char *Core_BackendName(void) { return EmuCore_BackendName(); }

const char *Core_StatusLine(void) {
    static char line[224];
    if (g_selftest_fails == 0)
        snprintf(line, sizeof(line), "%s; interpreter self-test: pass",
                 EmuCore_StatusLine());
    else if (g_selftest_fails > 0)
        snprintf(line, sizeof(line), "%s; interpreter self-test FAILED (%d: %s)",
                 EmuCore_StatusLine(), g_selftest_fails, EmuCore_SelfTestReport());
    else
        snprintf(line, sizeof(line), "%s", EmuCore_StatusLine());
    return line;
}

void Core_Init(void) {
    EmuCore_Init();
    g_selftest_fails = EmuCore_SelfTest();
}

void Core_Shutdown(void) { /* nothing to tear down */ }

int Core_IsActive(void) { return EmuCore_Active(); }

int Core_CanBoot(const Game *g) {
    return g && g->container == CONT_NSO;
}

const char *Core_WhyNot(const Game *g) {
    if (!g) return "No title selected.";
    switch (g->container) {
    case CONT_NSO:
        return "Standalone module: press A to load and run it on the interpreter.";
    case CONT_NSP:
        return "NSP container: the file list can be read, but NCAs need prod.keys to decrypt.";
    case CONT_NRO:
        return "NRO homebrew: the interpreter is wired, but only NSO images load today.";
    default:
        return "Unrecognised container: nothing to run.";
    }
}

/* Reads a file and runs it through the bridge. Kept small and bounded so a
 * huge container can never exhaust the console's RAM. */
int Core_Boot(const Game *g, char *msg, int msg_sz) {
    char path[200];
    if (!msg || msg_sz <= 0) return 0;
    msg[0] = '\0';
    if (!g) { snprintf(msg, msg_sz, "No title selected."); return 0; }

    if (g->dir[0]) snprintf(path, sizeof(path), "%s/%s", g->dir, g->file);
    else           snprintf(path, sizeof(path), "%s", g->file);

    if (!Fs_Ready()) { snprintf(msg, msg_sz, "Storage is not mounted."); return 0; }
    EmuCore_Init();

    if (g->container == CONT_NSO) {
        /* Allocate the read buffer from the heap instead of a 4 MB static:
         * a big static here pushes libXenon's low .bss (and its EHCI DMA
         * structures) up past the window the 360's USB controller can reach,
         * which makes mass storage vanish. The heap is large and this is used
         * only after storage is already mounted. */
        uint8_t *buf = (uint8_t *)malloc(4u * 1024u * 1024u);
        long n;
        if (!buf) { snprintf(msg, msg_sz, "Out of memory for NSO buffer"); return 0; }
        n = Fs_ReadFile(path, buf, 4u * 1024u * 1024u);
        if (n <= 0) { free(buf); snprintf(msg, msg_sz, "Cannot read %s", g->file); return 0; }
        if (!EmuCore_LoadNso(buf, (unsigned long)n)) {
            free(buf);
            snprintf(msg, msg_sz,
                     "NSO load failed: bad/unsupported header or outside guest RAM");
            return 0;
        }
        free(buf);
        int stopped = EmuCore_RunSlice(2000000u);
        snprintf(msg, msg_sz, "Ran NSO: entry=0x%08x insns=%u -> %s",
                 (unsigned)EmuCore_Entry(), (unsigned)EmuCore_InstrCount(),
                 stopped ? EmuCore_HaltName() : "budget reached, still running");
        return 1;
    }

    if (g->container == CONT_NSP) {
        static uint8_t hdr[65536];
        long n = Fs_ReadFile(path, hdr, (uint32_t)sizeof(hdr));
        if (n <= 0) { snprintf(msg, msg_sz, "Cannot read %s", g->file); return 0; }
        const char *reason = 0;
        int cnt = EmuCore_ProbePfs0(hdr, (unsigned long)n,
                                    (unsigned long long)g->size, &reason);
        if (cnt < 0) {
            snprintf(msg, msg_sz, "NSP parse failed: %s", reason ? reason : "unknown");
            return 0;
        }
        snprintf(msg, msg_sz,
                 "NSP: %d file(s). NCA decryption requires prod.keys; not booted.", cnt);
        return 0;
    }

    snprintf(msg, msg_sz, "%s", Core_WhyNot(g));
    return 0;
}

/* ---- capability model -------------------------------------------------- */

static const CoreCapability kCaps[CAP_COUNT] = {
    { "CPU",      CAPSTATE_BETA,    "AArch64 interpreter (emucore/a64): integer/logic/move/branch/load-store; runs NSO entry code." },
    { "GPU",      CAPSTATE_BETA,    "Xbox 360 Xenos 2D framebuffer output via libXenon; no guest Tegra/Maxwell 3D yet." },
    { "Memory",   CAPSTATE_BETA,    "8 MB guest RAM provisioned; bounds-checked, faults are reported." },
    { "Firmware", CAPSTATE_STUB,    "No Horizon OS/services; SVCs dispatch to an honest, mostly-empty HLE registry." },
    { "Loader",   CAPSTATE_BETA,    "PFS0 + NSO parsed and loaded and executed; NCA decryption needs prod.keys." },
    { "Audio",    CAPSTATE_NONE,    "No audio output at all." },
};

int Core_CapabilityCount(void) { return CAP_COUNT; }

const CoreCapability *Core_CapabilityAt(int i) {
    if (i < 0 || i >= CAP_COUNT) return 0;
    return &kCaps[i];
}

const char *Core_CapStateName(CoreCapState s) {
    switch (s) {
    case CAPSTATE_STUB:    return "stub";
    case CAPSTATE_PARTIAL: return "partial";
    case CAPSTATE_BETA:    return "beta";
    case CAPSTATE_READY:   return "ready";
    default:               return "not implemented";
    }
}

/* ---- feasibility ------------------------------------------------------- */

static const CoreRequirement kReq[] = {
    { "Guest RAM",  "4 GB LPDDR4",                 "512 MB GDDR3 (shared)", 0 },
    { "Guest CPU",  "8x ARM Cortex-A57 + 4x A53",  "3x PowerPC Xenon",      0 },
    { "Guest GPU",  "NVIDIA Maxwell (Tegra X1)",   "ATI Xenos",             0 },
    { "Guest OS",   "Horizon (boot0 + sysmodules)","libXenon (bare-metal)", 0 },
    { "Browse UI",  "n/a",                         "works",                 1 },
};

int Core_RequirementCount(void) { return (int)(sizeof(kReq) / sizeof(kReq[0])); }

const CoreRequirement *Core_RequirementAt(int i) {
    if (i < 0 || i >= Core_RequirementCount()) return 0;
    return &kReq[i];
}

const char *Core_Feasibility(void) {
    return "Beta: the AArch64 interpreter, guest RAM and the PFS0/NSO loaders "
           "run on real Xbox 360 hardware, so standalone NSO modules execute. "
           "Retail NSP titles still need prod.keys (NCA decryption) plus a "
           "GPU/OS, so they are not bootable yet.";
}
