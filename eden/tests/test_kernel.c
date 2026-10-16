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
        printf("PASS: kernel (QueryMemory, ticks, debug log, heap, exit)\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", fails);
    return 1;
}
