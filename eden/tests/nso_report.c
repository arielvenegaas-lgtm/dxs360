/* nso_report.c: load a SYNTHETIC NSO into the interpreter and print exactly
 * what happened - entry PC, instruction count, halt reason, SVC/HLE counts and
 * any memory fault. This is a controlled, synthetic test image; it is NOT Cave
 * Story+ and does not use any retail content.
 *
 * Build (repo root):
 *   gcc -std=c99 -Wall -Wextra -O1 -I emucore -I emucore/a64 -I emucore/hle -I emucore/loader \
 *       eden/tests/nso_report.c emucore/a64/a64.c emucore/a64/a64_mem.c \
 *       emucore/hle/hle.c emucore/loader/pfs0.c emucore/loader/nso.c emucore/loader/loader.c \
 *       -o /tmp/nso_report && /tmp/nso_report
 */
#include <stdio.h>
#include <string.h>

#include "a64.h"
#include "a64_mem.h"
#include "hle.h"
#include "nso.h"
#include "loader.h"

/* The synthetic image: text/rodata/data at fixed guest locations (all within
 * the small guest window the frontend provisions). */
#define TEXT_LOC 0x1000u
#define RO_LOC   0x2000u
#define DATA_LOC 0x3000u
#define SCRATCH  0x4000u   /* where the test stores to / loads from */

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Build an NSO whose text segment holds `insns` (already little-endian words). */
static uint64_t build_nso(uint8_t *buf, const uint32_t *insns, int n) {
    uint32_t text_off = 0x100, ro_off = (uint32_t)(0x100 + n * 4), data_off = ro_off + 4;
    uint32_t text_sz = (uint32_t)(n * 4);
    memset(buf, 0, data_off + 8);
    memcpy(buf, "NSO0", 4);
    put32(buf + 0x10, text_off); put32(buf + 0x14, 0x00001000u); put32(buf + 0x18, text_sz); put32(buf + 0x1C, 0);
    put32(buf + 0x20, ro_off);   put32(buf + 0x24, 0x00002000u); put32(buf + 0x28, 4);       put32(buf + 0x2C, 0);
    put32(buf + 0x30, data_off); put32(buf + 0x34, 0x00003000u); put32(buf + 0x38, 8);       put32(buf + 0x3C, 16);
    for (int i = 0; i < n; ++i) put32(buf + text_off + i * 4, insns[i]);
    put32(buf + ro_off, 0x11223344u);
    put32(buf + data_off, 0x55667788u); put32(buf + data_off + 4, 0x99AABBCCu);
    return data_off + 8;
}

static int svc0(void *ctx, A64 *cpu, uint64_t imm) {
    (void)ctx; (void)imm;
    A64_SetX(cpu, 0, 0x42u);
    return 1;
}

int main(void) {
    static uint8_t gmem[64u << 20];
    static uint8_t nsbuf[4096];
    Nso nso; LoadedImage img; A64GuestMem mem; A64 cpu; HleState hle;
    int fails = 0;

    A64GuestMem_Init(&mem, gmem, sizeof(gmem), 0);
    A64_Init(&cpu, A64GuestMem_Read, A64GuestMem_Write, &mem);

    /* ---- Scenario 1: store, load, then an unhandled SVC ---- */
    {
        const uint32_t prog[] = {
            0xD2880000u, /* movz x0, #0x4000 */
            0xD29579A1u, /* movz x1, #0xABCD */
            0xB9000001u, /* str  w1, [x0]     -> mem[0x4000]=0xABCD */
            0xB9400002u, /* ldr  w2, [x0]     -> x2 = 0xABCD */
            0xD4000001u, /* svc  #0           -> x8=0, unknown -> halt */
        };
        uint64_t sz = build_nso(nsbuf, prog, 5);
        printf("== synthetic NSO: store / load / unhandled SVC ==\n");
        if (Nso_Parse(&nso, nsbuf, sz) != NSO_OK) { printf("  NSO parse FAILED\n"); ++fails; }
        else if (Nso_LoadIntoGuest(&nso, nsbuf, sz, &mem, &img) != LOAD_OK) { printf("  load FAILED\n"); ++fails; }
        else {
            Hle_Init(&hle);
            A64_Reset(&cpu, img.entry, 1);
            A64_SetSvcHandler(&cpu, Hle_HandleSvc, &hle);
            A64Halt h = A64_Run(&cpu, 100000u);
            uint8_t raw[4] = { 0, 0, 0, 0 };
            A64GuestMem_Read(&mem, 0x4000u, raw, 4);
            uint64_t stored = (uint64_t)raw[0] | ((uint64_t)raw[1] << 8) |
                              ((uint64_t)raw[2] << 16) | ((uint64_t)raw[3] << 24);
            printf("  entry=0x%08x  icount=%llu  halt=%s\n",
                   (unsigned)img.entry, (unsigned long long)cpu.icount, A64_HaltName(h));
            printf("  x2(loaded)=0x%llx  mem[0x4000]=0x%llx  fault=0x%llx/%llu/w%d\n",
                   (unsigned long long)A64_GetX(&cpu,2), (unsigned long long)stored,
                   (unsigned long long)cpu.fault_addr, (unsigned long long)cpu.fault_size, cpu.fault_write);
            printf("  svc seen=%llu handled=%llu unknown=%llu last_unknown=0x%llx\n",
                   (unsigned long long)hle.calls, (unsigned long long)hle.handled,
                   (unsigned long long)hle.unknown_calls, (unsigned long long)hle.last_unknown);
            if (h != A64_HALT_SVC) { printf("  expected svc halt\n"); ++fails; }
            if (A64_GetX(&cpu,2) != 0xABCDu) { printf("  expected x2=0xABCD\n"); ++fails; }
            if (stored != 0xABCDu) { printf("  expected mem=0xABCD\n"); ++fails; }
            if (hle.calls != 1 || hle.handled != 0 || hle.unknown_calls != 1) { printf("  svc counters wrong\n"); ++fails; }
        }
    }

    /* ---- Scenario 2: a registered SVC is handled, then BRK ---- */
    {
        const uint32_t prog[] = {
            0xD2800008u, /* movz x8, #0        (svc call number 0) */
            0xD4000001u, /* svc  #0            -> handled, x0=0x42 */
            0xD4200000u, /* brk  #0 */
        };
        uint64_t sz = build_nso(nsbuf, prog, 3);
        printf("== synthetic NSO: handled SVC then BRK ==\n");
        if (Nso_Parse(&nso, nsbuf, sz) != NSO_OK) { printf("  NSO parse FAILED\n"); ++fails; }
        else if (Nso_LoadIntoGuest(&nso, nsbuf, sz, &mem, &img) != LOAD_OK) { printf("  load FAILED\n"); ++fails; }
        else {
            Hle_Init(&hle);
            Hle_Register(&hle, 0, "test.setx0", svc0, 0);
            A64_Reset(&cpu, img.entry, 1);
            A64_SetSvcHandler(&cpu, Hle_HandleSvc, &hle);
            A64Halt h = A64_Run(&cpu, 100000u);
            printf("  entry=0x%08x  icount=%llu  halt=%s  x0=0x%llx\n",
                   (unsigned)img.entry, (unsigned long long)cpu.icount, A64_HaltName(h),
                   (unsigned long long)A64_GetX(&cpu,0));
            printf("  svc seen=%llu handled=%llu unknown=%llu\n",
                   (unsigned long long)hle.calls, (unsigned long long)hle.handled,
                   (unsigned long long)hle.unknown_calls);
            if (h != A64_HALT_BRK) { printf("  expected brk halt\n"); ++fails; }
            if (A64_GetX(&cpu,0) != 0x42u) { printf("  expected x0=0x42\n"); ++fails; }
            if (hle.handled != 1 || hle.unknown_calls != 0) { printf("  expected handled=1\n"); ++fails; }
        }
    }

    /* ---- Scenario 3: out-of-range load and store (memory faults) ---- */
    {
        printf("== synthetic NSO: out-of-range memory (faults) ==\n");
        const uint32_t ld[] = { 0xB9400001u }; /* ldr w1, [x0] */
        const uint32_t st[] = { 0xB9000002u }; /* str w2, [x0] */
        uint64_t sz = build_nso(nsbuf, ld, 1);
        Nso_Parse(&nso, nsbuf, sz); Nso_LoadIntoGuest(&nso, nsbuf, sz, &mem, &img);
        A64_Reset(&cpu, img.entry, 1);
        A64_SetX(&cpu, 0, 0xFFFFFFFFu); /* far outside the guest RAM window */
        A64Halt h = A64_Run(&cpu, 10u);
        printf("  load : halt=%s fault_addr=0x%llx size=%llu write=%d\n",
               A64_HaltName(h), (unsigned long long)cpu.fault_addr,
               (unsigned long long)cpu.fault_size, cpu.fault_write);
        if (h != A64_HALT_MEMFAULT || cpu.fault_write != 0 || cpu.fault_size != 4) { printf("  unexpected load fault\n"); ++fails; }

        sz = build_nso(nsbuf, st, 1);
        Nso_Parse(&nso, nsbuf, sz); Nso_LoadIntoGuest(&nso, nsbuf, sz, &mem, &img);
        A64_Reset(&cpu, img.entry, 1);
        A64_SetX(&cpu, 0, 0xFFFFFFFFu);
        h = A64_Run(&cpu, 10u);
        printf("  store: halt=%s fault_addr=0x%llx size=%llu write=%d\n",
               A64_HaltName(h), (unsigned long long)cpu.fault_addr,
               (unsigned long long)cpu.fault_size, cpu.fault_write);
        if (h != A64_HALT_MEMFAULT || cpu.fault_write != 1 || cpu.fault_size != 4) { printf("  unexpected store fault\n"); ++fails; }
    }

    printf(fails ? "FAIL: %d check(s)\n" : "PASS: synthetic NSO report\n", fails);
    return fails ? 1 : 0;
}
