/* Host test: safe guest memory + interpreter fault reporting.
 *
 * Build (from repo root):
 *   gcc -std=c99 -Wall -Wextra -I emucore/a64 \
 *       eden/tests/test_a64_mem.c emucore/a64/a64.c emucore/a64/a64_mem.c \
 *       -o /tmp/test_a64_mem && /tmp/test_a64_mem
 */
#include <stdio.h>

#include "a64.h"
#include "a64_mem.h"

static int fails;

#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++fails; } } while (0)

static uint8_t g_buf[4096];

int main(void) {
    A64GuestMem mem;
    A64 cpu;

    A64GuestMem_Init(&mem, g_buf, sizeof(g_buf), 0x10000000u);

    CHECK(A64GuestMem_Host(&mem, 0x10000000u, 1) == g_buf, "host base maps to buffer");
    CHECK(A64GuestMem_Host(&mem, 0x10000000u + 4095, 1) != NULL, "host last byte maps");
    CHECK(A64GuestMem_Host(&mem, 0x10000000u + 4096, 1) == NULL, "host one past end faults");
    CHECK(A64GuestMem_Host(&mem, 0x0FFFFFFFu, 1) == NULL, "host below base faults");
    CHECK(A64GuestMem_Host(&mem, ~(uint64_t)0, 8) == NULL, "host overflow faults");

    {
        uint32_t v = 0xDEADBEEFu, got = 0;
        CHECK(A64GuestMem_Write(&mem, 0x10000010u, &v, 4) == 1, "write ok");
        CHECK(A64GuestMem_Read(&mem, 0x10000010u, &got, 4) == 1, "read ok");
        CHECK(got == v, "read/write roundtrip");
        CHECK(A64GuestMem_Read(&mem, 0x20000000u, &got, 4) == 0, "out-of-range read faults");
    }

    /* Interpreter integration: a real load reported with metadata. */
    A64GuestMem_Attach(&mem, &cpu);
    {
        /* movz x0,#0x2000,lsl#16 ; ldr x1,[x0]  (little-endian guest bytes) */
        const uint8_t prog[8] = { 0x00,0x00,0xA4,0xD2, 0x01,0x00,0x40,0xF9 };
        A64GuestMem_Write(&mem, 0x10000000u, (void *)prog, sizeof(prog));
        A64_Reset(&cpu, 0x10000000u, 1);
        CHECK(A64_Step(&cpu) == A64_OK, "step 1 (movz) ok");
        CHECK(cpu.x[0] == 0x20000000u, "movz produced unmapped address");
        CHECK(A64_Step(&cpu) == A64_HALT_MEMFAULT, "load from unmapped faults");
        CHECK(cpu.fault_addr == 0x20000000u, "fault_addr recorded");
        CHECK(cpu.fault_size == 8, "fault_size recorded");
        CHECK(cpu.fault_write == 0, "fault direction is read");
    }

    /* Interpreter integration: a real store reported as a write fault. */
    {
        const uint8_t prog[8] = { 0x00,0x00,0xA4,0xD2, 0x01,0x00,0x00,0xF9 }; /* movz x0,#0x2000,lsl16 ; str x1,[x0] */
        A64GuestMem_Write(&mem, 0x10000020u, (void *)prog, sizeof(prog));
        A64_Reset(&cpu, 0x10000020u, 1);
        CHECK(A64_Step(&cpu) == A64_OK, "step 1 (movz) ok");
        CHECK(A64_Step(&cpu) == A64_HALT_MEMFAULT, "store to unmapped faults");
        CHECK(cpu.fault_addr == 0x20000000u, "store fault_addr recorded");
        CHECK(cpu.fault_write == 1, "fault direction is write");
    }

    if (fails == 0) {
        printf("PASS: a64_mem (bounds, overflow, roundtrip, interpreter fault metadata)\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", fails);
    return 1;
}
