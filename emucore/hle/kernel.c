/* kernel.c: Horizon kernel-call (SVC) HLE handlers. */
#include "kernel.h"

#include <string.h>

/* Syscall numbers (Horizon). */
#define SVC_SET_HEAP_SIZE       0x01u
#define SVC_QUERY_MEMORY        0x06u
#define SVC_EXIT_PROCESS        0x07u
#define SVC_GET_SYSTEM_TICK     0x1Eu
#define SVC_OUTPUT_DEBUG_STRING 0x27u
#define SVC_GET_PROCESS_ID      0x24u

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) {
    int i;
    for (i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i));
}

static const HleKernelRegion *FindRegion(const HleKernel *k, uint64_t addr) {
    int i;
    for (i = 0; i < k->count; ++i) {
        const HleKernelRegion *r = &k->regions[i];
        if (addr >= r->base && addr < r->base + r->size) return r;
    }
    return NULL;
}

/* The unmapped span containing `addr`: bounded by the nearest mapped region
 * above/below within [0, span_end). */
static void UnmappedSpan(const HleKernel *k, uint64_t addr,
                         uint64_t *lo, uint64_t *hi) {
    uint64_t l = 0, h = k->span_end;
    int i;
    for (i = 0; i < k->count; ++i) {
        const HleKernelRegion *r = &k->regions[i];
        if (r->base + r->size <= addr && r->base + r->size > l) l = r->base + r->size;
        if (r->base > addr && r->base < h) h = r->base;
        (void)r;
    }
    *lo = l; *hi = h;
}

void HleKernel_Init(HleKernel *k) {
    memset(k, 0, sizeof(*k));
    k->span_end = 0;
    k->log = NULL;
}

void HleKernel_SetSpanEnd(HleKernel *k, uint64_t end) {
    k->span_end = end;
}

void HleKernel_SetLog(HleKernel *k, HleKernelLog fn, void *ctx) {
    k->log = fn;
    k->log_ctx = ctx;
}

int HleKernel_AddRegion(HleKernel *k, uint64_t base, uint64_t size,
                        uint32_t type, uint32_t perm) {
    if (size == 0 || k->count >= HLEK_MAX_REGIONS) return 0;
    k->regions[k->count].base = base;
    k->regions[k->count].size = size;
    k->regions[k->count].type = type;
    k->regions[k->count].perm = perm;
    ++k->count;
    return 1;
}

/* ---- handlers ---- */

/* QueryMemory(MemoryInfo *out_meminfo [x0], PageInfo *out_pageinfo [x1],
 *             u64 addr [x2]) -> Result in X0, PageInfo in X1.
 *
 * The MemoryInfo struct is horizon's 0x28-byte record:
 *   +0x00 u64 addr, +0x08 u64 size, +0x10 u32 type,
 *   +0x14 u32 attributes, +0x18 u32 permission, ... */
static int svcQueryMemory(void *ctx, A64 *cpu, uint64_t svc) {
    HleKernel *k = (HleKernel *)ctx;
    uint64_t out = cpu->x[0];
    uint64_t addr = cpu->x[2];
    uint8_t mi[0x28];
    uint32_t pageinfo;
    const HleKernelRegion *r;
    (void)svc;

    memset(mi, 0, sizeof(mi));
    r = FindRegion(k, addr);
    if (r) {
        put64(mi + 0, r->base);
        put64(mi + 8, r->size);
        put32(mi + 16, r->type);
        put32(mi + 20, 0);
        put32(mi + 24, r->perm);
        pageinfo = 4; /* mapped (4 KiB granule marker) */
    } else {
        uint64_t lo, hi;
        UnmappedSpan(k, addr, &lo, &hi);
        put64(mi + 0, lo);
        put64(mi + 8, hi > lo ? hi - lo : 0);
        put32(mi + 16, HLEK_MEM_UNMAPPED);
        put32(mi + 24, 0);
        pageinfo = 0;
    }
    if (cpu->write == NULL || !cpu->write(cpu->memctx, out, mi, sizeof(mi)))
        return 0; /* unhandled -> fault/halt reported */
    cpu->x[0] = 0;   /* Result: success */
    cpu->x[1] = pageinfo;
    ++k->query_count;
    return 1;
}

static int svcGetSystemTick(void *ctx, A64 *cpu, uint64_t svc) {
    HleKernel *k = (HleKernel *)ctx;
    (void)svc;
    cpu->x[0] = k->tick++;
    return 1;
}

/* libnx: svcGetProcessId(u64 *out [x0], u64 pid [x1]) writes the process id
 * and returns a Result. We report the current process as pid 0x1000. */
static int svcGetProcessId(void *ctx, A64 *cpu, uint64_t svc) {
    uint8_t buf[8];
    uint64_t out = cpu->x[0];
    (void)ctx; (void)svc;
    put64(buf, 0x1000);
    if (!cpu->write || !cpu->write(cpu->memctx, out, buf, sizeof(buf)))
        return 0;
    cpu->x[0] = 0;
    return 1;
}

static int svcOutputDebugString(void *ctx, A64 *cpu, uint64_t svc) {
    HleKernel *k = (HleKernel *)ctx;
    char buf[256];
    uint64_t p = cpu->x[0];
    int n = 0;
    (void)svc;
    if (cpu->read) {
        while (n < (int)sizeof(buf) - 1) {
            char ch;
            if (!cpu->read(cpu->memctx, p + (uint64_t)n, &ch, 1)) break;
            if (ch == 0) break;
            buf[n++] = ch;
        }
    }
    buf[n] = 0;
    if (k->log) k->log(k->log_ctx, buf);
    cpu->x[0] = 0;
    return 1;
}

static int svcExitProcess(void *ctx, A64 *cpu, uint64_t svc) {
    HleKernel *k = (HleKernel *)ctx;
    (void)cpu; (void)svc;
    k->exited = 1;
    /* Deliberately unhandled: the program asked to exit, which is a real event
     * the caller must see (the CPU halts and the reason is reported). */
    return 0;
}

static int svcSetHeapSize(void *ctx, A64 *cpu, uint64_t svc) {
    HleKernel *k = (HleKernel *)ctx;
    /* libnx: svcSetHeapSize(void **out [x0], size_t size [x1]). */
    uint64_t size = cpu->x[1];
    uint64_t base;
    uint8_t buf[8];
    (void)svc;
    if (size == 0 || k->span_end == 0) return 0;
    base = (k->span_end - size) & ~(uint64_t)0xFFF;
    k->heap_base = base;
    k->heap_size = size;
    put64(buf, base);
    if (!cpu->write || !cpu->write(cpu->memctx, cpu->x[0], buf, sizeof(buf)))
        return 0;
    HleKernel_AddRegion(k, base, size, HLEK_MEM_HEAP, HLEK_PERM_R | HLEK_PERM_W);
    cpu->x[0] = 0;
    return 1;
}

void HleKernel_Register(HleState *h, HleKernel *k) {
    Hle_Register(h, SVC_QUERY_MEMORY,        "QueryMemory",       svcQueryMemory, k);
    Hle_Register(h, SVC_GET_SYSTEM_TICK,     "GetSystemTick",     svcGetSystemTick, k);
    Hle_Register(h, SVC_OUTPUT_DEBUG_STRING, "OutputDebugString", svcOutputDebugString, k);
    Hle_Register(h, SVC_SET_HEAP_SIZE,       "SetHeapSize",       svcSetHeapSize, k);
    Hle_Register(h, SVC_GET_PROCESS_ID,      "GetProcessId",      svcGetProcessId, k);
    Hle_Register(h, SVC_EXIT_PROCESS,        "ExitProcess",       svcExitProcess, k);
}

const char *HleKernel_SvcName(uint64_t svc) {
    switch (svc) {
        case SVC_SET_HEAP_SIZE:       return "SetHeapSize";
        case SVC_QUERY_MEMORY:        return "QueryMemory";
        case SVC_EXIT_PROCESS:        return "ExitProcess";
        case SVC_GET_SYSTEM_TICK:     return "GetSystemTick";
        case SVC_OUTPUT_DEBUG_STRING: return "OutputDebugString";
        case SVC_GET_PROCESS_ID:      return "GetProcessId";
        default:                      return "?";
    }
}
