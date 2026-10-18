/* kernel.c: Horizon kernel-call (SVC) HLE handlers. */
#include "kernel.h"
#include "ipc.h"

#include <stdio.h>
#include <string.h>

/* Syscall numbers (Horizon). */
#define SVC_SET_HEAP_SIZE       0x01u
#define SVC_QUERY_MEMORY        0x06u
#define SVC_EXIT_PROCESS        0x07u
#define SVC_SIGNAL_PROCESS_KEY  0x1Du
#define SVC_GET_SYSTEM_TICK     0x1Eu
#define SVC_CREATE_TRANSFER_MEM 0x15u
#define SVC_CLOSE_HANDLE        0x16u
#define SVC_WAIT_SYNCHRONIZATION 0x18u
#define SVC_CONNECT_NAMED_PORT  0x1Fu
#define SVC_SEND_SYNC_REQUEST   0x20u
#define SVC_SEND_SYNC_UBUF      0x21u
#define SVC_GET_THREAD_PRIORITY 0x0Cu
#define SVC_GET_PROCESS_ID      0x24u
#define SVC_GET_THREAD_ID       0x25u
#define SVC_OUTPUT_DEBUG_STRING 0x27u
#define SVC_GET_INFO            0x29u

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
    k->process_id = 0x1000;
    k->next_handle = 0x20;
    k->applet_evt = -1;
    /* The am service is already holding the initial focus transition when an
     * application first runs its message loop, so queue it now: without it the
     * SDK's pump waits for a focus notification that would never arrive. */
    HleKernel_PushAppletMessage(k, HLEK_APPLET_MSG_FOCUS_STATE_CHANGED);
}

void HleKernel_SetSpanEnd(HleKernel *k, uint64_t end) {
    k->span_end = end;
}

void HleKernel_SetLog(HleKernel *k, HleKernelLog fn, void *ctx) {
    k->log = fn;
    k->log_ctx = ctx;
}

void HleKernel_SetHeapMapper(HleKernel *k, HleKernelHeapMap map,
                             HleKernelHeapUsed used, void *ctx) {
    k->heap_map = map;
    k->heap_used = used;
    k->heap_ctx = ctx;
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
    uint64_t base, size;
    uint32_t type, perm;
    const HleKernelRegion *r;
    HleKernelQuery *q;
    (void)svc;

    memset(mi, 0, sizeof(mi));
    r = FindRegion(k, addr);
    if (r) {
        base = r->base; size = r->size; type = r->type; perm = r->perm;
        pageinfo = 4; /* mapped (4 KiB granule marker) */
    } else {
        uint64_t lo, hi;
        UnmappedSpan(k, addr, &lo, &hi);
        base = lo; size = hi > lo ? hi - lo : 0;
        type = HLEK_MEM_UNMAPPED; perm = 0;
        pageinfo = 0;
    }

    put64(mi + 0, base);
    put64(mi + 8, size);
    put32(mi + 16, type);
    put32(mi + 20, 0);
    put32(mi + 24, perm);
    if (cpu->write == NULL || !cpu->write(cpu->memctx, out, mi, sizeof(mi)))
        return 0; /* unhandled -> fault/halt reported */

    /* Record the probe for diagnostics before reporting success. */
    q = &k->qtrace[k->qtrace_next];
    q->addr = addr;
    q->base = base;
    q->size = size;
    q->type = type;
    q->perm = perm;
    k->qtrace_next = (k->qtrace_next + 1) % HLEK_QUERY_TRACE;
    if (k->qtrace_count < HLEK_QUERY_TRACE) ++k->qtrace_count;

    cpu->x[0] = 0;   /* Result: success */
    cpu->x[1] = pageinfo;
    ++k->query_count;
    return 1;
}

int HleKernel_QueryTraceCount(const HleKernel *k) {
    return k->qtrace_count;
}

int HleKernel_QueryTraceAt(const HleKernel *k, int i, HleKernelQuery *out) {
    int idx;
    if (i < 0 || i >= k->qtrace_count) return 0;
    /* Oldest first: once the ring has wrapped, entry 0 is at qtrace_next. */
    idx = (k->qtrace_next - k->qtrace_count + i + HLEK_QUERY_TRACE) % HLEK_QUERY_TRACE;
    *out = k->qtrace[idx];
    return 1;
}

const char *HleKernel_QueryVerdict(const HleKernelQuery *q) {
    /* rtld's module-discovery loop (the walker behind
     * MapUtils::LocateSpaceForMapDeprecated) accepts a region iff it is
     * CodeStatic with R|X, and stops the whole scan on type 0x10. */
    if (q->type == 0x10u) return "scan stops (type 0x10 terminator)";
    if (q->type == HLEK_MEM_CODE_STATIC && q->perm == (HLEK_PERM_R | HLEK_PERM_X))
        return "accepted: CodeStatic+R|X (module candidate)";
    if (q->type == HLEK_MEM_UNMAPPED) return "unmapped: scan steps over";
    return "skipped: not CodeStatic+R|X";
}

static int svcGetSystemTick(void *ctx, A64 *cpu, uint64_t svc) {
    HleKernel *k = (HleKernel *)ctx;
    (void)svc;
    cpu->x[0] = k->tick++;
    return 1;
}

/* svcGetProcessId / svcGetThreadId return their 64-bit value in X1, with the
 * Result in W0 (the out-pointer the SDK passes in X0 is stored by the SDK's own
 * wrapper, not written here -- see the SDK SVC stub: `str x0,[sp,#-16]!; svc
 * #N; ldr x2,[sp]; str x1,[x2]`). */
static int svcGetProcessId(void *ctx, A64 *cpu, uint64_t svc) {
    HleKernel *k = (HleKernel *)ctx;
    (void)svc;
    cpu->x[0] = 0;               /* Result: success */
    cpu->x[1] = k->process_id;   /* ProcessId       */
    return 1;
}

/* svcGetThreadPriority(Handle thread [x1]) -> Result in W0, priority in W1.
 * A single-threaded emulated process reports one stable default priority. */
#define HLEK_DEFAULT_PRIORITY 0x2Cu
static int svcGetThreadPriority(void *ctx, A64 *cpu, uint64_t svc) {
    uint64_t handle = cpu->x[1];
    (void)ctx; (void)svc; (void)handle;
    cpu->x[0] = 0;                     /* Result   */
    cpu->x[1] = HLEK_DEFAULT_PRIORITY; /* Priority */
    return 1;
}

/* svcSignalProcessWideKey(cv_key addr [x0], int32 count [x1]) -> Result in W0.
 * A process-wide "waiter" wake (futex-style). The emulated process runs one
 * thread, so there is never a waiter to wake; the call succeeds and does
 * nothing. Real multi-thread wake-up is out of scope until threads exist. */
static int svcSignalProcessWideKey(void *ctx, A64 *cpu, uint64_t svc) {
    (void)ctx; (void)svc;
    cpu->x[0] = 0; /* Result: success (no waiters) */
    return 1;
}

/* svcConnectToNamedPort(const char *name [x1]) -> Result in W0, session
 * Handle in W1. The kernel (service manager) owns the ports; this layer just
 * allocates a session handle and remembers which port it names so the
 * following IPC can be handled with full context. */
static int svcConnectToNamedPort(void *ctx, A64 *cpu, uint64_t svc) {
    HleKernel *k = (HleKernel *)ctx;
    uint64_t name_ptr = cpu->x[1];
    char name[HLEK_SESSION_NAME];
    uint64_t h;
    int n = 0;
    (void)svc;

    if (cpu->read) {
        while (n < (int)sizeof(name) - 1) {
            char ch;
            if (!cpu->read(cpu->memctx, name_ptr + (uint64_t)n, &ch, 1)) break;
            if (ch == 0) break;
            name[n++] = ch;
        }
    }
    name[n] = 0;

    {
        HleKernelSession *s = HleKernel_NewSession(k, name,
            strcmp(name, "sm:") == 0 ? HLEK_OBJ_SM : HLEK_OBJ_NONE);
        h = s ? s->handle : k->next_handle++;
    }
    if (k->log) {
        char line[HLEK_SESSION_NAME + 48];
        snprintf(line, sizeof(line), "ConnectToNamedPort(\"%s\") -> handle 0x%llx",
                 name, (unsigned long long)h);
        k->log(k->log_ctx, line);
    }

    cpu->x[0] = 0;  /* Result        */
    cpu->x[1] = h;  /* SessionHandle */
    return 1;
}

const char *HleKernel_SessionName(const HleKernel *k, uint64_t handle) {
    int i;
    for (i = 0; i < k->session_count; ++i)
        if (k->sessions[i].handle == handle) return k->sessions[i].name;
    return NULL;
}

HleKernelSession *HleKernel_FindSession(HleKernel *k, uint64_t handle) {
    int i;
    for (i = 0; i < k->session_count; ++i)
        if (k->sessions[i].handle == handle) return &k->sessions[i];
    return NULL;
}

HleKernelSession *HleKernel_NewSession(HleKernel *k, const char *name,
                                       uint32_t object) {
    HleKernelSession *s;
    int i;
    if (k->session_count >= HLEK_MAX_SESSIONS) return NULL;
    s = &k->sessions[k->session_count++];
    memset(s, 0, sizeof(*s));
    s->handle = k->next_handle++;
    s->object = object;
    for (i = 0; i < HLEK_SESSION_NAME - 1 && name[i]; ++i) s->name[i] = name[i];
    s->name[i] = 0;
    return s;
}

/* Publish a new interface inside a session's domain. Object ids are unique
 * only within the owning session (see CmifDomainOutHeader). */
uint32_t HleKernel_NewDomainObj(HleKernel *k, HleKernelSession *s,
                                const char *name) {
    HleKernelDomainObj *d = NULL;
    uint32_t id;
    int i;

    for (i = 0; i < HLEK_MAX_DOMAIN_OBJS && !d; ++i)
        if (!k->domain_objs[i].in_use) d = &k->domain_objs[i];
    if (!d) return 0;

    id = s->next_obj_id ? s->next_obj_id : 2;
    if (id == 0) id = 1;

    d->in_use = 1;
    d->session = s->domain_root ? s->domain_root : s->handle;
    d->obj_id = id;
    for (i = 0; i < HLEK_SESSION_NAME - 1 && name[i]; ++i) d->name[i] = name[i];
    d->name[i] = 0;
    s->next_obj_id = id + 1;
    return id;
}

/* Interface name for an object id inside `s`'s domain, or NULL if unknown. */
const char *HleKernel_FindDomainObj(const HleKernel *k,
                                    const HleKernelSession *s, uint32_t obj_id) {
    int i;
    uint64_t root;
    if (s->is_domain && obj_id == s->domain_id) return s->name;
    root = s->domain_root ? s->domain_root : s->handle;
    for (i = 0; i < HLEK_MAX_DOMAIN_OBJS; ++i)
        if (k->domain_objs[i].in_use &&
            k->domain_objs[i].session == root &&
            k->domain_objs[i].obj_id == obj_id)
            return k->domain_objs[i].name;
    return NULL;
}

/* Allocate a signallable event, drawing its handle from the shared allocator. */
HleKernelEvent *HleKernel_NewEvent(HleKernel *k, const char *name,
                                   int autoclear) {
    HleKernelEvent *e = NULL;
    int i;
    for (i = 0; i < HLEK_MAX_EVENTS && !e; ++i)
        if (!k->events[i].in_use) e = &k->events[i];
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));
    e->in_use = 1;
    e->handle = k->next_handle++;
    e->signaled = 0;
    e->autoclear = autoclear;
    for (i = 0; i < HLEK_SESSION_NAME - 1 && name[i]; ++i) e->name[i] = name[i];
    e->name[i] = 0;
    return e;
}

HleKernelTransferMem *HleKernel_FindTransferMem(HleKernel *k, uint64_t handle) {
    int i;
    for (i = 0; i < HLEK_MAX_TRANSFER_MEM; ++i)
        if (k->tmem[i].in_use && k->tmem[i].handle == handle)
            return &k->tmem[i];
    return NULL;
}

HleKernelEvent *HleKernel_FindEvent(HleKernel *k, uint64_t handle) {
    int i;
    for (i = 0; i < HLEK_MAX_EVENTS; ++i)
        if (k->events[i].in_use && k->events[i].handle == handle)
            return &k->events[i];
    return NULL;
}

/* Keep the bound event's signalled state in step with the queue depth: the real
 * am service signals it while a message is pending and clears it once drained. */
static void applet_sync_event(HleKernel *k) {
    HleKernelEvent *e = HleKernel_AppletEvent(k);
    if (e) e->signaled = (k->applet_count > 0);
}

HleKernelEvent *HleKernel_AppletEvent(HleKernel *k) {
    if (k->applet_evt < 0 || k->applet_evt >= HLEK_MAX_EVENTS) return NULL;
    if (!k->events[k->applet_evt].in_use) return NULL;
    return &k->events[k->applet_evt];
}

HleKernelEvent *HleKernel_BindAppletEvent(HleKernel *k, HleKernelEvent *e) {
    if (!e) return NULL;
    k->applet_evt = (int)(e - k->events);
    applet_sync_event(k);
    return e;
}

void HleKernel_PushAppletMessage(HleKernel *k, uint32_t msg) {
    if (k->applet_count >= HLEK_MAX_APPLET_MSGS) return;  /* queue full */
    k->applet_msgs[(k->applet_head + k->applet_count) % HLEK_MAX_APPLET_MSGS] = msg;
    ++k->applet_count;
    applet_sync_event(k);
}

int HleKernel_PopAppletMessage(HleKernel *k, uint32_t *out) {
    if (k->applet_count <= 0) { applet_sync_event(k); return 0; }
    *out = k->applet_msgs[k->applet_head];
    k->applet_head = (k->applet_head + 1) % HLEK_MAX_APPLET_MSGS;
    --k->applet_count;
    applet_sync_event(k);
    return 1;
}

/* svcSendSyncRequest(handle [x0])                       -- TLS message buffer
 * svcSendSyncRequestWithUserBuffer(buf [x0], size [x1], handle [x2])
 *   (official kernel ABI per switchbrew) -- but real SDK builds are observed
 *   calling the WithUserBuffer form as (handle, buf, size). Resolve by which
 *   register names a live session, and which looks like a page-aligned guest
 *   pointer/its size. The TLS buffer (svcSendSyncRequest) lives at
 *   tpidr_el0 + 0x100 (ThreadLocalRegion::ipcBuffer). */
static int svcSendSyncRequest(void *ctx, A64 *cpu, uint64_t svc) {
    HleKernel *k = (HleKernel *)ctx;
    uint64_t a = cpu->x[0], b = cpu->x[1], c = cpu->x[2];
    uint64_t handle, buf, size;

    (void)svc;
    if (HleKernel_FindSession(k, a)) {
        handle = a; buf = b; size = c;
    } else if (HleKernel_FindSession(k, c)) {
        handle = c; buf = a; size = b;
    } else if (HleKernel_FindSession(k, b)) {
        handle = b; buf = c; size = a;
    } else {
        handle = a; buf = b; size = c;
    }
    if (svc == SVC_SEND_SYNC_REQUEST) {
        /* TLS buffer: ThreadLocalRegion + 0x100, message size 0x100. */
        buf = cpu->tpidr_el0 + 0x100;
        size = 0x100;
        handle = a;
    }

    if (k->log) {
        char line[80];
        snprintf(line, sizeof(line),
                 "SendSyncRequest(handle 0x%llx buf 0x%llx size 0x%llx)",
                 (unsigned long long)handle, (unsigned long long)buf,
                 (unsigned long long)size);
        k->log(k->log_ctx, line);
    }
    if (HleIpc_SendSyncRequest(k, cpu, handle, buf, size)) {
        cpu->x[0] = 0;   /* Result: success (HIPC-level) */
        return 1;
    }
    if (k->log) k->log(k->log_ctx, HleIpc_LastUnhandled());
    return 0;
}

static int svcGetThreadId(void *ctx, A64 *cpu, uint64_t svc) {
    HleKernel *k = (HleKernel *)ctx;
    (void)svc;
    cpu->x[0] = 0;                     /* Result: success */
    cpu->x[1] = k->process_id + 1;     /* thread id (synthetic, stable) */
    return 1;
}

/* splitmix64: deterministic pseudo-random source for GetInfo(RandomEntropy).
 * The kernel returns real entropy; an emulator returns a stable, reproducible
 * value so a given title behaves identically across runs. */
static uint64_t splitmix64(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* First region of `type` (by base order of insertion); returns 1 if found. */
static int FindRegionByType(const HleKernel *k, uint32_t type,
                            uint64_t *base, uint64_t *size) {
    int i;
    for (i = 0; i < k->count; ++i) {
        if (k->regions[i].type == type) {
            *base = k->regions[i].base;
            *size = k->regions[i].size;
            return 1;
        }
    }
    return 0;
}

/* Lowest mapped base and total mapped bytes (UsedMemorySize / ASLR region). */
static void RegionBounds(const HleKernel *k, uint64_t *lo, uint64_t *sum) {
    uint64_t l = 0, s = 0;
    int i, first = 1;
    for (i = 0; i < k->count; ++i) {
        const HleKernelRegion *r = &k->regions[i];
        if (first || r->base < l) { l = r->base; first = 0; }
        s += r->size;
    }
    *lo = l; *sum = s;
}

/* svcGetInfo(u64 *unused_x0, InfoType type [x1], Handle handle [x2],
 *            u64 subtype [x3]) -> Result in W0, Info value in X1. */
static int svcGetInfo(void *ctx, A64 *cpu, uint64_t svc) {
    HleKernel *k = (HleKernel *)ctx;
    uint64_t type = cpu->x[1];
    uint64_t handle = cpu->x[2];
    uint64_t subtype = cpu->x[3];
    uint64_t value = 0;
    uint64_t lo, sum, base, size;
    (void)handle; (void)svc;

    switch (type) {
    case HLEK_INFO_RANDOM_ENTROPY:
        if (!k->entropy_ready) {
            uint64_t seed = 0x123456789ABCDEF0ULL;
            int i;
            for (i = 0; i < 4; ++i) k->entropy[i] = splitmix64(&seed);
            k->entropy_ready = 1;
        }
        value = k->entropy[subtype & 3u];
        break;

    case HLEK_INFO_DEBUGGER_ATTACHED:  value = 0; break;
    case HLEK_INFO_IDLE_TICK_COUNT:    value = 0; break;
    case HLEK_INFO_IS_APPLICATION:     value = 1; break;
    case HLEK_INFO_FREE_THREAD_COUNT:  value = 0x40; break;
    case HLEK_INFO_SYSRESOURCE_TOTAL:
    case HLEK_INFO_SYSRESOURCE_USED:   value = 0; break;

    case HLEK_INFO_INITIAL_PID_RANGE:
        value = (subtype == 0) ? 0x1000u : 0x2000u;
        break;

    case HLEK_INFO_TOTAL_MEMORY_SIZE:
    case HLEK_INFO_ASLR_REGION_SIZE:
        RegionBounds(k, &lo, &sum);
        value = k->span_end > lo ? k->span_end - lo : 0;
        break;
    case HLEK_INFO_USED_MEMORY_SIZE: {
        /* The process's committed memory: every mapped region except the heap
         * reservation (which is address space, not storage), plus the bytes of
         * the demand-paged heap the backend has actually backed. The flat
         * modules/stack/TLS reservation counts in full so the SDK's
         * Total-Used heap sizing lands exactly on the heap window base. */
        int i;
        sum = 0;
        for (i = 0; i < k->count; ++i)
            if (k->regions[i].type != HLEK_MEM_HEAP)
                sum += k->regions[i].size;
        if (k->heap_used) sum += k->heap_used(k->heap_ctx);
        value = sum;
        break;
    }
    case HLEK_INFO_ASLR_REGION_ADDR:
        RegionBounds(k, &lo, &sum);
        value = lo;
        break;

    case HLEK_INFO_HEAP_REGION_ADDR:
        value = k->heap_base;
        break;
    case HLEK_INFO_HEAP_REGION_SIZE:
        value = k->heap_size;
        break;

    case HLEK_INFO_STACK_REGION_ADDR:
        if (!FindRegionByType(k, HLEK_MEM_STACK, &base, &size)) return 0;
        value = base;
        break;
    case HLEK_INFO_STACK_REGION_SIZE:
        if (!FindRegionByType(k, HLEK_MEM_STACK, &base, &size)) return 0;
        value = size;
        break;

    default:
        /* Unknown / unsupported id0: decline so the stop reports the exact
         * InfoType instead of returning a fabricated value. */
        return 0;
    }

    cpu->x[0] = 0;      /* Result: success */
    cpu->x[1] = value;  /* Info            */
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
    if (size == 0) return 0;
    if (k->heap_map) {
        /* Backend with a demand-paged heap window: let it place the reservation.
         * The requested size is mapped exactly (never silently shrunk), so the
         * guest's own accounting stays valid. */
        if (!k->heap_map(k->heap_ctx, size, &base)) return 0;
    } else {
        if (k->span_end == 0) return 0;
        base = (k->span_end - size) & ~(uint64_t)0xFFF;
    }
    k->heap_base = base;
    k->heap_size = size;
    put64(buf, base);
    if (!cpu->write || !cpu->write(cpu->memctx, cpu->x[0], buf, sizeof(buf)))
        return 0;
    {   /* Resize the existing heap region in place, or add the first one. */
        int i, replaced = 0;
        for (i = 0; i < k->count; ++i) {
            if (k->regions[i].type == HLEK_MEM_HEAP) {
                k->regions[i].base = base;
                k->regions[i].size = size;
                replaced = 1;
                break;
            }
        }
        if (!replaced)
            HleKernel_AddRegion(k, base, size, HLEK_MEM_HEAP,
                                HLEK_PERM_R | HLEK_PERM_W);
    }
    /* The SDK wrapper (str x0,[sp,#-16]!; svc #1; ldr x2,[sp]; str x1,[x2])
     * returns the heap base in X1; X0 carries the Result. */
    cpu->x[1] = base;
    cpu->x[0] = 0;
    return 1;
}

/* svcWaitSynchronization. Two call shapes are modelled:
 *
 *  - WaitSynchronization(Handle handle [x0], s64 timeout [x1]);
 *  - the SDK's multi-wait wrapper, observed calling with
 *      x0 = out-index pointer, x1 = handle-array pointer, x2 = count,
 *      x3 = timeout (s64, -1 = infinite)
 *    (its stub is `str x0,[sp,#-16]!; svc #0x18; ldr x2,[sp]; str w1,[x2]`,
 *    i.e. the kernel returns the signalled value in x1 and the wrapper stores
 *    it through the caller's out pointer).
 *
 * The wait succeeds only for a modelled event that is currently signalled (the
 * applet message event is signalled exactly while a message is pending). An
 * unsignalled event, or a handle that is not a modelled event, is left
 * unhandled so the stop surfaces honestly rather than being faked. */
static int svcWaitSynchronization(void *ctx, A64 *cpu, uint64_t svc) {
    HleKernel *k = (HleKernel *)ctx;
    uint32_t h = 0, zero = 0;
    HleKernelEvent *e;
    int multi = 0;
    (void)svc;

    e = HleKernel_FindEvent(k, cpu->x[0]);
    if (!e && cpu->read && cpu->read(cpu->memctx, cpu->x[1], &h, 4)) {
        e = HleKernel_FindEvent(k, h);
        if (e) multi = 1;
    }
    if (!e) return 0;   /* not a modelled event: leave unhandled */

    if (!e->signaled) {
        /* Horizon would suspend this thread until someone signals it. Nothing
         * in this emulator produces that signal and there is no other runnable
         * thread to switch to, so the wait can never be satisfied. Decline the
         * instance so the interpreter halts here and the reason is logged,
         * instead of pretending the guest woke up. */
        if (k->log) {
            char line[160];
            snprintf(line, sizeof(line),
                     "WaitSynchronization BLOCKED: event '%s' (handle 0x%llx) "
                     "is unsignalled and has no producer",
                     e->name, (unsigned long long)e->handle);
            k->log(k->log_ctx, line);
        }
        return 0;
    }

    if (e->autoclear) e->signaled = 0;
    if (multi) {
        if (cpu->write) cpu->write(cpu->memctx, cpu->x[0], &zero, 4);
        cpu->x[1] = 0;   /* signalled index (x1; the SDK wrapper stores it) */
    }
    cpu->x[0] = 0;       /* Result: success */
    return 1;
}

/* svcCreateTransferMemory(Handle *out [x0], void *addr [x1], size_t size [x2],
 *                         u32 perm [x3]) -> Result in X0, Handle in X1.
 * The kernel validates the range (non-empty, page-aligned) and mints a handle
 * so the object can be handed to a service; the object is tracked like an event
 * so CloseHandle and diagnostics see something real. */
static int svcCreateTransferMemory(void *ctx, A64 *cpu, uint64_t svc) {
    HleKernel *k = (HleKernel *)ctx;
    uint64_t addr = cpu->x[1];
    uint64_t size = cpu->x[2];
    uint32_t perm = (uint32_t)cpu->x[3];
    uint32_t handle;
    uint8_t buf[4];
    int i, slot = -1;
    (void)svc;
    if (size == 0) return 0;
    if ((addr & 0xFFF) || (size & 0xFFF)) {
        /* Real RefreshTransferMemory/Create require page alignment; decline so
         * the stop reports the call rather than accepting an impossible range. */
        if (k->log) {
            char line[128];
            snprintf(line, sizeof(line),
                     "CreateTransferMemory addr=0x%llx size=0x%llx not page-aligned",
                     (unsigned long long)addr, (unsigned long long)size);
            k->log(k->log_ctx, line);
        }
        return 0;
    }
    for (i = 0; i < HLEK_MAX_TRANSFER_MEM; ++i)
        if (!k->tmem[i].in_use) { slot = i; break; }
    if (slot < 0) return 0;
    handle = (uint32_t)k->next_handle++;
    k->tmem[slot].in_use = 1;
    k->tmem[slot].handle = handle;
    k->tmem[slot].addr = addr;
    k->tmem[slot].size = size;
    k->tmem[slot].perm = perm;
    put32(buf, handle);
    if (!cpu->write || !cpu->write(cpu->memctx, cpu->x[0], buf, sizeof(buf)))
        return 0;
    if (k->log) {
        char line[128];
        snprintf(line, sizeof(line),
                 "CreateTransferMemory(addr=0x%llx size=0x%llx perm=%u) -> handle 0x%x",
                 (unsigned long long)addr, (unsigned long long)size, perm, handle);
        k->log(k->log_ctx, line);
    }
    cpu->x[1] = handle;
    cpu->x[0] = 0;
    return 1;
}

static int svcCloseHandle(void *ctx, A64 *cpu, uint64_t svc) {
    HleKernel *k = (HleKernel *)ctx;
    uint64_t handle = cpu->x[0];
    HleKernelSession *s = HleKernel_FindSession(k, handle);
    HleKernelEvent *e = HleKernel_FindEvent(k, handle);
    HleKernelTransferMem *tm = HleKernel_FindTransferMem(k, handle);
    int i;
    (void)svc;
    if (s) {
        /* Remove the session (compact the array). */
        int idx = (int)(s - k->sessions);
        for (i = idx; i + 1 < k->session_count; ++i)
            k->sessions[i] = k->sessions[i + 1];
        memset(&k->sessions[k->session_count - 1], 0, sizeof(k->sessions[0]));
        --k->session_count;
        cpu->x[0] = 0;
        return 1;
    }
    if (e) {
        /* Release the event; its handle is not reused. */
        memset(e, 0, sizeof(*e));
        cpu->x[0] = 0;
        return 1;
    }
    if (tm) {
        /* Release the transfer-memory object; its handle is not reused. */
        memset(tm, 0, sizeof(*tm));
        cpu->x[0] = 0;
        return 1;
    }
    /* Other kernel objects (threads, ...) are not modelled; accept the close
     * so the client's bookkeeping proceeds. */
    if (k->log) {
        char line[64];
        snprintf(line, sizeof(line), "CloseHandle(0x%llx) [untracked]",
                 (unsigned long long)handle);
        k->log(k->log_ctx, line);
    }
    cpu->x[0] = 0;
    return 1;
}

void HleKernel_Register(HleState *h, HleKernel *k) {
    Hle_Register(h, SVC_QUERY_MEMORY,        "QueryMemory",       svcQueryMemory, k);
    Hle_Register(h, SVC_SIGNAL_PROCESS_KEY,  "SignalProcessWideKey", svcSignalProcessWideKey, k);
    Hle_Register(h, SVC_GET_SYSTEM_TICK,     "GetSystemTick",     svcGetSystemTick, k);
    Hle_Register(h, SVC_CREATE_TRANSFER_MEM, "CreateTransferMemory", svcCreateTransferMemory, k);
    Hle_Register(h, SVC_CLOSE_HANDLE,        "CloseHandle",       svcCloseHandle, k);
    Hle_Register(h, SVC_WAIT_SYNCHRONIZATION, "WaitSynchronization", svcWaitSynchronization, k);
    Hle_Register(h, SVC_CONNECT_NAMED_PORT,  "ConnectToNamedPort", svcConnectToNamedPort, k);
    Hle_Register(h, SVC_SEND_SYNC_REQUEST,   "SendSyncRequest",   svcSendSyncRequest, k);
    Hle_Register(h, SVC_SEND_SYNC_UBUF,      "SendSyncRequestWithUserBuffer", svcSendSyncRequest, k);
    Hle_Register(h, SVC_OUTPUT_DEBUG_STRING, "OutputDebugString", svcOutputDebugString, k);
    Hle_Register(h, SVC_SET_HEAP_SIZE,       "SetHeapSize",       svcSetHeapSize, k);
    Hle_Register(h, SVC_GET_THREAD_PRIORITY, "GetThreadPriority", svcGetThreadPriority, k);
    Hle_Register(h, SVC_GET_PROCESS_ID,      "GetProcessId",      svcGetProcessId, k);
    Hle_Register(h, SVC_GET_THREAD_ID,       "GetThreadId",       svcGetThreadId, k);
    Hle_Register(h, SVC_GET_INFO,            "GetInfo",           svcGetInfo, k);
    Hle_Register(h, SVC_EXIT_PROCESS,        "ExitProcess",       svcExitProcess, k);
}

const char *HleKernel_SvcName(uint64_t svc) {
    switch (svc) {
        case SVC_SET_HEAP_SIZE:       return "SetHeapSize";
        case SVC_QUERY_MEMORY:        return "QueryMemory";
        case SVC_EXIT_PROCESS:        return "ExitProcess";
        case SVC_GET_SYSTEM_TICK:     return "GetSystemTick";
        case SVC_CREATE_TRANSFER_MEM: return "CreateTransferMemory";
        case SVC_CLOSE_HANDLE:        return "CloseHandle";
        case SVC_WAIT_SYNCHRONIZATION: return "WaitSynchronization";
        case SVC_SIGNAL_PROCESS_KEY:  return "SignalProcessWideKey";
        case SVC_CONNECT_NAMED_PORT:  return "ConnectToNamedPort";
        case SVC_SEND_SYNC_REQUEST:   return "SendSyncRequest";
        case SVC_SEND_SYNC_UBUF:      return "SendSyncRequestWithUserBuffer";
        case SVC_OUTPUT_DEBUG_STRING: return "OutputDebugString";
        case SVC_GET_THREAD_PRIORITY: return "GetThreadPriority";
        case SVC_GET_PROCESS_ID:      return "GetProcessId";
        case SVC_GET_THREAD_ID:       return "GetThreadId";
        case SVC_GET_INFO:            return "GetInfo";
        default:                      return "?";
    }
}
