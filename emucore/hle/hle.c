/* hle.c: honest AArch64 SVC -> HLE dispatch bridge. */
#include "hle.h"

#include <string.h>

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

    /* The Switch kernel ABI is SVC #0 only; anything else is not ours. */
    if (imm != 0) {
        ++h->unknown_calls;
        h->last_unknown = imm;
        return 0;
    }

    svc = cpu->x[8];
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
