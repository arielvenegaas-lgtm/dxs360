/* core.cpp: see core.h. Honest stubs only. */
#include "core.h"

const char *Core_BackendName(void) {
    return "none (frontend build)";
}

const char *Core_StatusLine(void) {
    return "Emulation core not implemented - library browsing only";
}

int Core_CanBoot(const Game *g) {
    (void)g;
    return 0; /* nothing is bootable in this milestone */
}

const char *Core_WhyNot(const Game *g) {
    (void)g;
    return "This build only browses game files; it does not emulate the console.";
}
