/* core.h: reserved for a future emulation core.
 *
 * This build is a frontend only. The functions below deliberately report that
 * nothing can be booted yet; no CPU, GPU, memory or firmware emulation is
 * faked. A real core (should one ever be feasible on the Xbox 360's PowerPC
 * hardware under libXenon) would implement this interface.
 */
#pragma once

#include "library.h"

/* Backend identifier, e.g. "none (frontend build)". */
const char *Core_BackendName(void);

/* One-line status for the About screen / status panel. */
const char *Core_StatusLine(void);

/* Whether this build can boot `g`. Always 0 for now. */
int Core_CanBoot(const Game *g);

/* Human-readable reason a title cannot be booted. */
const char *Core_WhyNot(const Game *g);
