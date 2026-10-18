/* Host-side test for the Horizon kernel-call (SVC) HLE handlers.
 *
 * Build (from the repository root):
 *   gcc -std=c99 -Wall -Wextra -I emucore/a64 -I emucore/hle \
 *       eden/tests/test_kernel.c emucore/a64/a64.c emucore/a64/a64_mem.c \
 *       emucore/hle/hle.c emucore/hle/kernel.c -o test_kernel && ./test_kernel
 */
#include <stdio.h>
#include <string.h>
#include "a64.h"
#include "a64_mem.h"
#include "hle.h"
#include "kernel.h"

#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++fails; } } while (0)

#define GUEST_BASE 0x10000000u

static uint8_t g_buf[4096];

static char g_log[256];
static int  g_log_calls;
static void my_log(void *ctx, const char *s) {
    (void)ctx;
    snprintf(g_log, sizeof(g_log), "%s", s);
    ++g_log_calls;
}

/* Heap mapper used by the SetHeapSize tests: records the requested size, places
 * the heap at a fixed base (or rejects when g_hm_reject is set), and reports a
 * caller-controlled committed byte count. */
#define HM_BASE 0x30000000ULL
static uint64_t g_hm_size, g_hm_used;
static int      g_hm_reject;
static int hm_map(void *ctx, uint64_t size, uint64_t *out_base) {
    (void)ctx;
    if (g_hm_reject) return 0;
    g_hm_size = size;
    *out_base = HM_BASE;
    return 1;
}
static uint64_t hm_used(void *ctx) { (void)ctx; return g_hm_used; }

static uint32_t rd32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static uint64_t rd64(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    uint64_t v = 0; int i;
    for (i = 7; i >= 0; --i) v = (v << 8) | b[i];
    return v;
}

/* Guest-memory helpers: the IPC message buffer lives at GUEST_BASE + 0x800. */
#define IPCBUF 0x800

static void wr32(A64GuestMem *mem, uint32_t off, uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t)v;         b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
    A64GuestMem_Write(mem, GUEST_BASE + off, b, 4);
}
static uint32_t rd32g(A64GuestMem *mem, uint32_t off) {
    uint8_t b[4];
    A64GuestMem_Read(mem, GUEST_BASE + off, b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* Send a sync request for `handle` (observed ABI: x0=handle, x1=buf, x2=dup). */
static int send_req(A64 *cpu, HleState *hle, uint64_t handle) {
    A64_Reset(cpu, 0x1000, 1);
    cpu->x[0] = handle;
    cpu->x[1] = GUEST_BASE + IPCBUF;
    cpu->x[2] = handle;
    return Hle_HandleSvc(hle, cpu, 0x21);
}

int main(void) {
    int fails = 0;
    A64GuestMem mem;
    A64 cpu;
    HleState hle;
    HleKernel k;
    uint8_t mi[0x28];

    A64GuestMem_Init(&mem, g_buf, sizeof(g_buf), GUEST_BASE);
    A64GuestMem_Attach(&mem, &cpu);
    Hle_Init(&hle);
    HleKernel_Init(&k);
    HleKernel_SetSpanEnd(&k, 0x20000);
    HleKernel_SetLog(&k, my_log, NULL);
    HleKernel_Register(&hle, &k);

    /* A code region and a stack region. */
    CHECK(HleKernel_AddRegion(&k, 0x1000, 0x2000, HLEK_MEM_CODE_STATIC,
                              HLEK_PERM_R | HLEK_PERM_X), "add code region");
    CHECK(HleKernel_AddRegion(&k, 0x4000, 0x1000, HLEK_MEM_STACK,
                              HLEK_PERM_R | HLEK_PERM_W), "add stack region");

    /* QueryMemory(syscall 6): x0=out, x1=pageinfo out, x2=addr. */
    A64_Reset(&cpu, 0x1000, 1);
    cpu.x[0] = GUEST_BASE;      /* MemoryInfo out */
    cpu.x[1] = 0;               /* PageInfo out (returned in X1) */
    cpu.x[2] = 0x1100;          /* an address inside the code region */
    CHECK(Hle_HandleSvc(&hle, &cpu, 0x6) == 1, "QueryMemory handled");
    CHECK(cpu.x[0] == 0, "QueryMemory success result");
    CHECK(cpu.x[1] != 0, "QueryMemory pageinfo non-zero for mapped");
    A64GuestMem_Read(&mem, GUEST_BASE, mi, sizeof(mi));
    CHECK(rd64(mi + 0) == 0x1000, "mapped: base");
    CHECK(rd64(mi + 8) == 0x2000, "mapped: size");
    CHECK(rd32(mi + 16) == HLEK_MEM_CODE_STATIC, "mapped: type code");
    CHECK(rd32(mi + 24) == (HLEK_PERM_R | HLEK_PERM_X), "mapped: perm r-x");

    /* Query an unmapped address between the two regions. */
    A64_Reset(&cpu, 0x1000, 1);
    cpu.x[0] = GUEST_BASE;
    cpu.x[2] = 0x3500;
    CHECK(Hle_HandleSvc(&hle, &cpu, 0x6) == 1, "QueryMemory(unmapped) handled");
    A64GuestMem_Read(&mem, GUEST_BASE, mi, sizeof(mi));
    CHECK(rd64(mi + 0) == 0x3000, "unmapped: base = end of code region");
    CHECK(rd64(mi + 8) == 0x1000, "unmapped: size to next region");
    CHECK(rd32(mi + 16) == HLEK_MEM_UNMAPPED, "unmapped: type");

    /* OutputDebugString(0x27): captures the guest NUL-terminated string. */
    {
        const char *msg = "hello from guest";
        A64GuestMem_Write(&mem, GUEST_BASE + 0x100, (void *)msg, strlen(msg) + 1);
        A64_Reset(&cpu, 0x1000, 1);
        cpu.x[0] = GUEST_BASE + 0x100;
        CHECK(Hle_HandleSvc(&hle, &cpu, 0x27) == 1, "OutputDebugString handled");
        CHECK(g_log_calls == 1 && strcmp(g_log, msg) == 0, "log captured");
    }

    /* GetSystemTick(0x1E): monotonic. */
    A64_Reset(&cpu, 0x1000, 1);
    CHECK(Hle_HandleSvc(&hle, &cpu, 0x1E) == 1, "GetSystemTick handled");
    CHECK(cpu.x[0] == 0, "first tick is 0");
    A64_Reset(&cpu, 0x1000, 1);
    CHECK(Hle_HandleSvc(&hle, &cpu, 0x1E) == 1, "GetSystemTick handled again");
    CHECK(cpu.x[0] == 1, "tick advances");

    /* SetHeapSize(0x01): x0=out, x1=size; reports a base and registers it. */
    A64_Reset(&cpu, 0x1000, 1);
    cpu.x[0] = GUEST_BASE;
    cpu.x[1] = 0x1000;
    CHECK(Hle_HandleSvc(&hle, &cpu, 0x1) == 1, "SetHeapSize handled");
    CHECK(cpu.x[0] == 0, "SetHeapSize success");
    {
        uint64_t base;
        A64GuestMem_Read(&mem, GUEST_BASE, &base, sizeof(base));
        CHECK(rd64(&base) == k.heap_base && k.heap_size == 0x1000, "heap recorded");
    }

    /* GetProcessId(0x24) / GetThreadId(0x25): value in X1, Result in X0. */
    A64_Reset(&cpu, 0x1000, 1);
    CHECK(Hle_HandleSvc(&hle, &cpu, 0x24) == 1, "GetProcessId handled");
    CHECK(cpu.x[0] == 0 && cpu.x[1] == 0x1000, "GetProcessId value in X1");
    A64_Reset(&cpu, 0x1000, 1);
    CHECK(Hle_HandleSvc(&hle, &cpu, 0x25) == 1, "GetThreadId handled");
    CHECK(cpu.x[0] == 0 && cpu.x[1] != 0, "GetThreadId value in X1");

    /* CreateTransferMemory(0x15)/CloseHandle(0x16): validate the range and
     * mint a real, tracked handle. */
    A64_Reset(&cpu, 0x1000, 1);
    cpu.x[0] = GUEST_BASE;   /* out handle ptr */
    cpu.x[1] = 0x13000;      /* addr (page aligned) */
    cpu.x[2] = 0x1000;       /* size */
    cpu.x[3] = 0;            /* perm */
    CHECK(Hle_HandleSvc(&hle, &cpu, 0x15) == 1, "CreateTransferMemory handled");
    CHECK(cpu.x[0] == 0 && cpu.x[1] != 0, "transfer handle returned in X1");
    CHECK(rd32g(&mem, 0) == (uint32_t)cpu.x[1], "handle written to guest out ptr");
    {
        uint32_t h = (uint32_t)cpu.x[1];
        CHECK(HleKernel_FindTransferMem(&k, h) != NULL, "transfer memory tracked");
        A64_Reset(&cpu, 0x1000, 1); cpu.x[0] = h;
        CHECK(Hle_HandleSvc(&hle, &cpu, 0x16) == 1, "CloseHandle(transfer mem) handled");
        CHECK(HleKernel_FindTransferMem(&k, h) == NULL, "transfer memory released");
    }
    A64_Reset(&cpu, 0x1000, 1);
    cpu.x[0] = GUEST_BASE; cpu.x[1] = 0x13001; cpu.x[2] = 0x1000;
    CHECK(Hle_HandleSvc(&hle, &cpu, 0x15) == 0, "misaligned transfer range declined");

    /* GetInfo(0x29): RandomEntropy is deterministic per subtype and non-zero. */
    {
        uint64_t e0, e1, e2;
        A64_Reset(&cpu, 0x1000, 1); cpu.x[1] = HLEK_INFO_RANDOM_ENTROPY; cpu.x[3] = 2;
        CHECK(Hle_HandleSvc(&hle, &cpu, 0x29) == 1, "GetInfo RandomEntropy handled");
        CHECK(cpu.x[0] == 0, "GetInfo result ok");
        e0 = cpu.x[1];
        A64_Reset(&cpu, 0x1000, 1); cpu.x[1] = HLEK_INFO_RANDOM_ENTROPY; cpu.x[3] = 2;
        Hle_HandleSvc(&hle, &cpu, 0x29); e1 = cpu.x[1];
        A64_Reset(&cpu, 0x1000, 1); cpu.x[1] = HLEK_INFO_RANDOM_ENTROPY; cpu.x[3] = 3;
        Hle_HandleSvc(&hle, &cpu, 0x29); e2 = cpu.x[1];
        CHECK(e0 == e1, "entropy stable for a subtype");
        CHECK(e0 != e2, "entropy differs across subtypes");
        CHECK(e0 != 0, "entropy non-zero");
    }

    /* GetInfo: scalars and regions derived from the map. */
    A64_Reset(&cpu, 0x1000, 1); cpu.x[1] = HLEK_INFO_IS_APPLICATION;
    CHECK(Hle_HandleSvc(&hle, &cpu, 0x29) == 1 && cpu.x[1] == 1, "IsApplication");
    A64_Reset(&cpu, 0x1000, 1); cpu.x[1] = HLEK_INFO_STACK_REGION_ADDR;
    CHECK(Hle_HandleSvc(&hle, &cpu, 0x29) == 1 && cpu.x[1] == 0x4000, "StackRegionAddr");
    A64_Reset(&cpu, 0x1000, 1); cpu.x[1] = HLEK_INFO_STACK_REGION_SIZE;
    CHECK(Hle_HandleSvc(&hle, &cpu, 0x29) == 1 && cpu.x[1] == 0x1000, "StackRegionSize");
    A64_Reset(&cpu, 0x1000, 1); cpu.x[1] = HLEK_INFO_INITIAL_PID_RANGE; cpu.x[3] = 0;
    CHECK(Hle_HandleSvc(&hle, &cpu, 0x29) == 1 && cpu.x[1] == 0x1000, "InitialPidRange min");
    A64_Reset(&cpu, 0x1000, 1); cpu.x[1] = HLEK_INFO_HEAP_REGION_ADDR;
    CHECK(Hle_HandleSvc(&hle, &cpu, 0x29) == 1 && cpu.x[1] == k.heap_base, "HeapRegionAddr");
    A64_Reset(&cpu, 0x1000, 1); cpu.x[1] = 0x999;
    CHECK(Hle_HandleSvc(&hle, &cpu, 0x29) == 0, "unknown InfoType declined");

    /* SetHeapSize(0x01) with a heap mapper installed: the backend places the
     * reservation (a demand-paged window) and the existing heap region is
     * resized in place rather than duplicated. UsedMemorySize then reports the
     * non-heap regions plus the heap bytes actually backed. */
    {
        int i, heap_regions = 0;
        uint64_t used, hbase = 0, hsize = 0;
        HleKernel_SetHeapMapper(&k, hm_map, hm_used, NULL);
        A64_Reset(&cpu, 0x1000, 1);
        cpu.x[0] = GUEST_BASE;
        cpu.x[1] = 0x40000000ULL;
        CHECK(Hle_HandleSvc(&hle, &cpu, 0x1) == 1, "SetHeapSize via mapper handled");
        CHECK(g_hm_size == 0x40000000ULL, "mapper saw the requested size");
        CHECK(cpu.x[1] == HM_BASE, "mapper base returned in X1");
        A64GuestMem_Read(&mem, GUEST_BASE, &mi, 8);
        CHECK(rd64(mi) == HM_BASE, "mapper base written to guest");
        CHECK(k.heap_size == 0x40000000ULL, "heap size recorded");
        for (i = 0; i < k.count; ++i)
            if (k.regions[i].type == HLEK_MEM_HEAP) {
                ++heap_regions; hbase = k.regions[i].base; hsize = k.regions[i].size;
            }
        CHECK(heap_regions == 1, "heap region resized, not duplicated");
        CHECK(hbase == HM_BASE && hsize == 0x40000000ULL,
              "heap region moved to mapper base/size");

        g_hm_used = 0x5000;
        A64_Reset(&cpu, 0x1000, 1); cpu.x[1] = HLEK_INFO_USED_MEMORY_SIZE;
        CHECK(Hle_HandleSvc(&hle, &cpu, 0x29) == 1, "GetInfo UsedMemorySize handled");
        used = cpu.x[1];
        /* non-heap regions: code 0x2000 + stack 0x1000 = 0x3000, plus 0x5000. */
        CHECK(used == 0x8000, "UsedMemorySize = flat regions + heap committed");

        /* A mapper that rejects the size declines SetHeapSize (no fake base). */
        g_hm_reject = 1;
        A64_Reset(&cpu, 0x1000, 1); cpu.x[0] = GUEST_BASE; cpu.x[1] = 0x10;
        CHECK(Hle_HandleSvc(&hle, &cpu, 0x1) == 0, "rejected heap size declines call");
    }

    /* ExitProcess(0x07): observed but leaves the call unhandled so the
     * interpreter halts and reports the real reason. */
    A64_Reset(&cpu, 0x1000, 1);
    CHECK(Hle_HandleSvc(&hle, &cpu, 0x7) == 0, "ExitProcess leaves call unhandled");
    CHECK(k.exited == 1, "ExitProcess recorded");

    /* Unknown syscall number: not handled, reported. */
    A64_Reset(&cpu, 0x1000, 1);
    CHECK(Hle_HandleSvc(&hle, &cpu, 0x1234) == 0, "unknown syscall unhandled");
    CHECK(hle.last_unknown == 0x1234, "unknown syscall number recorded");

    if (fails == 0) {
        printf("PASS: kernel (QueryMemory, ticks, debug log, heap, GetInfo, ABI, exit)\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", fails);
    return 1;
}
