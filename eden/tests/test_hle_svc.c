/* Host-side test for the SVC -> HLE dispatch bridge.
 *
 * Build (from the repository root):
 *   gcc -std=c99 -Wall -Wextra -I emucore/a64 -I emucore/hle \
 *       eden/tests/test_hle_svc.c emucore/a64/a64.c emucore/a64/a64_mem.c \
 *       emucore/hle/hle.c -o test_hle_svc && ./test_hle_svc
 */
#include <stdio.h>
#include "a64.h"
#include "a64_mem.h"
#include "hle.h"

#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++fails; } } while (0)

static uint8_t g_buf[4096];
static int g_handler_calls;

static int my_handler(void *ctx, A64 *cpu, uint64_t svc) {
    (void)ctx; (void)svc;
    ++g_handler_calls;
    cpu->x[0] = 0x12345678u; /* "result" the guest would read back */
    return 1;
}

int main(void) {
    int fails = 0;
    A64GuestMem mem;
    A64 cpu;
    HleState hle;

    A64GuestMem_Init(&mem, g_buf, sizeof(g_buf), 0x10000000u);
    A64GuestMem_Attach(&mem, &cpu);

    Hle_Init(&hle);
    CHECK(Hle_Register(&hle, 0x1, "test.syscall", my_handler, NULL) == 1, "register handler");
    CHECK(Hle_Register(&hle, 0x1, "test.syscall", my_handler, NULL) == 1, "re-register replaces");
    CHECK(hle.count == 1, "handler count is 1 after replace");
    A64_SetSvcHandler(&cpu, Hle_HandleSvc, &hle);

    {
        const uint8_t svc0[4] = { 0x01, 0x00, 0x00, 0xD4 }; /* svc #0 (little-endian) */
        A64GuestMem_Write(&mem, 0x10000000u, (void *)svc0, sizeof(svc0));

        /* Known call: dispatched and result visible. */
        A64_Reset(&cpu, 0x10000000u, 1);
        cpu.x[8] = 0x1;
        CHECK(A64_Step(&cpu) == A64_OK, "known svc continues");
        CHECK(g_handler_calls == 1, "handler invoked once");
        CHECK(cpu.x[0] == 0x12345678u, "handler result in X0");
        CHECK(cpu.pc == 0x10000004u, "pc advanced past svc");
        CHECK(hle.handled == 1 && hle.unknown_calls == 0, "counters: handled");

        /* Unknown call: halted honestly, never faked. */
        A64_Reset(&cpu, 0x10000000u, 1);
        cpu.x[8] = 0x2;
        CHECK(A64_Step(&cpu) == A64_HALT_SVC, "unknown svc halts");
        CHECK(cpu.x[0] == 0, "unknown svc changed nothing");
        CHECK(hle.unknown_calls == 1 && hle.last_unknown == 0x2, "unknown recorded");

        /* Non-HLE immediate: rejected (Switch ABI is svc #0). */
        const uint8_t svc1[4] = { 0x21, 0x00, 0x00, 0xD4 }; /* svc #1 */
        A64GuestMem_Write(&mem, 0x10000000u, (void *)svc1, sizeof(svc1));
        A64_Reset(&cpu, 0x10000000u, 1);
        cpu.x[8] = 0x1;
        CHECK(A64_Step(&cpu) == A64_HALT_SVC, "svc #1 not treated as HLE");
        CHECK(hle.unknown_calls == 2 && hle.last_unknown == 1, "bad imm recorded");
    }

    /* No handler installed: SVC must halt, not silently continue. */
    {
        const uint8_t svc0[4] = { 0x01, 0x00, 0x00, 0xD4 };
        A64GuestMem_Write(&mem, 0x10000000u, (void *)svc0, sizeof(svc0));
        A64_Reset(&cpu, 0x10000000u, 1);
        A64_SetSvcHandler(&cpu, NULL, NULL);
        CHECK(A64_Step(&cpu) == A64_HALT_SVC, "no handler -> halt");
    }

    if (fails == 0) {
        printf("PASS: hle_svc (dispatch, unknown logging, no fake success)\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", fails);
    return 1;
}
