/* hle.h: honest AArch64 SVC -> HLE dispatch bridge.
 *
 * The Nintendo Switch kernel ABI is HLE'd. Native binaries issue `svc #N`
 * where N is the syscall number (e.g. #0x6 QueryMemory), with arguments in
 * X0..X7; the handler runs and leaves the result in X0. A bare `svc #0` is
 * the older convention that carries the call number in X8 and is still
 * accepted. This dispatcher implements only that plumbing plus a registry of
 * real handlers. It deliberately has NO built-in "pretend success" services:
 * an unknown call is counted and reported, and the caller decides what to do
 * (the interpreter halts with A64_HALT_SVC so a missing service can never be
 * mistaken for a working one).
 */
#pragma once

#include <stdint.h>
#include "a64.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Return 1 if the call was handled, 0 to leave it unhandled. */
typedef int (*HleFn)(void *ctx, A64 *cpu, uint64_t svc);

#define HLE_MAX_HANDLERS 64

typedef struct HleEntry {
    uint64_t svc;
    HleFn    fn;
    void    *ctx;
    const char *name;
} HleEntry;

typedef struct HleState {
    HleEntry handlers[HLE_MAX_HANDLERS];
    int      count;

    uint64_t calls;          /* SVC/HVC instructions seen                       */
    uint64_t handled;        /* dispatched to a registered handler             */
    uint64_t unknown_calls;  /* SVCs with no registered handler                */
    uint64_t last_unknown;   /* last unhandled call number                      */
    uint64_t last_imm;
} HleState;

void Hle_Init(HleState *h);

/* Register a handler for a Switch syscall number. Returns 1 on success. */
int Hle_Register(HleState *h, uint64_t svc, const char *name, HleFn fn, void *ctx);

/* A64SvcFn-compatible. Install with A64_SetSvcHandler(cpu, Hle_HandleSvc, h). */
int Hle_HandleSvc(void *ctx, A64 *cpu, uint64_t imm);

#ifdef __cplusplus
}
#endif
