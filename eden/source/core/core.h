/* core.h: the emulation-core interface and an honest capability model.
 *
 * This build is a FRONTEND ONLY. Nothing here emulates the Nintendo Switch,
 * and nothing fakes it either. The functions below report, truthfully, that no
 * CPU/GPU/memory/firmware execution exists yet, and they describe what a real
 * core would have to provide so the gap is visible instead of hidden.
 *
 * A future core would implement this interface (Core_Init / Core_CanBoot /
 * Core_Load); for now every entry point is a stub that says so.
 */
#pragma once

#include "library.h"

/* ---- identity / status ------------------------------------------------- */

/* Backend identifier, e.g. "none (frontend build)". */
const char *Core_BackendName(void);

/* One-line status for the About screen / status panel. */
const char *Core_StatusLine(void);

/* Lifecycle. Core_Init is a no-op today; it exists so a real core can start
 * subsystems without the UI changing. Core_IsActive is always 0 for now. */
void Core_Init(void);
void Core_Shutdown(void);
int  Core_IsActive(void);

/* Whether this build can boot `g`. Always 0 for now. */
int Core_CanBoot(const Game *g);

/* Human-readable reason a title cannot be booted. */
const char *Core_WhyNot(const Game *g);

/* ---- capability model -------------------------------------------------- */

typedef enum {
    CAP_CPU = 0,
    CAP_GPU,
    CAP_MEMORY,
    CAP_FIRMWARE,
    CAP_LOADER,
    CAP_AUDIO,
    CAP_COUNT
} CoreCapId;

typedef enum {
    CAPSTATE_NONE = 0,  /* nothing implemented                        */
    CAPSTATE_STUB,      /* interface only; no behaviour               */
    CAPSTATE_PARTIAL,   /* some real, non-executing functionality      */
    CAPSTATE_READY      /* fully working (never, in this build)        */
} CoreCapState;

typedef struct {
    const char *name;    /* "CPU", "GPU", ...            */
    CoreCapState state;  /* honest implementation level  */
    const char *detail;  /* one short line of truth      */
} CoreCapability;

int                 Core_CapabilityCount(void);
const CoreCapability *Core_CapabilityAt(int i);

/* Human-readable state name, e.g. "not implemented". */
const char *Core_CapStateName(CoreCapState s);

/* ---- feasibility ------------------------------------------------------- */

/* A requirement compares what a Switch title needs against what the Xbox 360
 * offers. Pure information; nothing is emulated. */
typedef struct {
    const char *name;   /* "Guest RAM", "Guest CPU", ... */
    const char *guest;  /* what the Switch provides      */
    const char *host;   /* what the Xbox 360 provides    */
    int         feasible;/* 1 if this looks achievable    */
} CoreRequirement;

int                   Core_RequirementCount(void);
const CoreRequirement *Core_RequirementAt(int i);

/* A single-line verdict about running Switch software on this console. */
const char *Core_Feasibility(void);
