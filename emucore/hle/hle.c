/* hle.c: honest AArch64 SVC -> HLE dispatch bridge. */
#include "hle.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static int hle_trace_on(void) {
    static int init = 0, on = 0;
    if (!init) { on = getenv("HLE_TRACE_SVC") != NULL; init = 1; }
    return on;
}

void Hle_Init(HleState *h) {
    memset(h, 0, sizeof(*h));
}

int Hle_Register(HleState *h, uint64_t svc, const char *name, HleFn fn, void *ctx) {
    int i;
    if (fn == NULL) return 0;
    for (i = 0; i < h->count; ++i) {
        if (h->handlers[i].svc == svc) { /* replace existing registration */
            h->handlers[i].fn = fn;
            h->handlers[i].ctx = ctx;
            h->handlers[i].name = name;
            return 1;
        }
    }
    if (h->count >= HLE_MAX_HANDLERS) return 0;
    h->handlers[h->count].svc = svc;
    h->handlers[h->count].fn = fn;
    h->handlers[h->count].ctx = ctx;
    h->handlers[h->count].name = name;
    ++h->count;
    return 1;
}

int Hle_HandleSvc(void *ctx, A64 *cpu, uint64_t imm) {
    HleState *h = (HleState *)ctx;
    uint64_t svc;
    int i;

    ++h->calls;
    for (i = 0; i < 8; ++i) h->last_args[i] = cpu->x[i];

    /* Real Horizon binaries issue `svc #N` where N is the syscall number
     * (e.g. #0x6 QueryMemory). A bare `svc #0` is the older HLE convention
     * that carries the call number in X8; support both so the same registry
     * serves native content and the synthetic tests. */
    svc = (imm != 0) ? imm : cpu->x[8];
    if (hle_trace_on())
        fprintf(stderr, "svc #0x%llx x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx lr=0x%llx\n",
                (unsigned long long)svc, (unsigned long long)cpu->x[0],
                (unsigned long long)cpu->x[1], (unsigned long long)cpu->x[2],
                (unsigned long long)cpu->x[3], (unsigned long long)cpu->x[30]);
    for (i = 0; i < h->count; ++i) {
        if (h->handlers[i].svc == svc) {
            if (h->handlers[i].fn(h->handlers[i].ctx, cpu, svc)) {
                ++h->handled;
                return 1;
            }
            /* handler exists but declined this instance */
            ++h->unknown_calls;
            h->last_unknown = svc;
            return 0;
        }
    }

    ++h->unknown_calls;
    h->last_unknown = svc;
    return 0;
}
