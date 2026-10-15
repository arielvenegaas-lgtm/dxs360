/* core.cpp: see core.h. Honest stubs + a truthful capability/feasibility model.
 *
 * No CPU, GPU, memory, audio or firmware emulation exists in this build. The
 * tables below exist so the UI can show, plainly, what is and is not done.
 */
#include "core.h"

/* ---- identity / status ------------------------------------------------- */

const char *Core_BackendName(void) {
    return "none (frontend build)";
}

const char *Core_StatusLine(void) {
    return "Emulation core not implemented - library browsing only";
}

void Core_Init(void)     { /* no subsystems to start yet */ }
void Core_Shutdown(void) { /* nothing to tear down        */ }
int  Core_IsActive(void) { return 0; }

int Core_CanBoot(const Game *g) {
    (void)g;
    return 0; /* nothing is bootable in this milestone */
}

const char *Core_WhyNot(const Game *g) {
    (void)g;
    return "This build only browses game files; it does not emulate the console.";
}

/* ---- capability model -------------------------------------------------- */

static const CoreCapability kCaps[CAP_COUNT] = {
    { "CPU",      CAPSTATE_NONE,    "No PowerPC/ARM execution; nothing is interpreted or JITed." },
    { "GPU",      CAPSTATE_NONE,    "No Tegra/Maxwell GPU emulation; only the 2D UI framebuffer." },
    { "Memory",   CAPSTATE_STUB,    "Guest memory map is described, but no guest RAM is allocated." },
    { "Firmware", CAPSTATE_NONE,    "No Horizon OS, services or sysmodules (no boot0/HOS)." },
    { "Loader",   CAPSTATE_PARTIAL, "Real container/module headers are decoded; no code is run." },
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
    return "Full Switch emulation is not feasible here: 3 PowerPC cores and "
           "512 MB RAM versus 8 ARM cores and 4 GB. Eden stays a browser.";
}
