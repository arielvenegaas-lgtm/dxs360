/* a64_test_entry.c: deterministic AArch64 interpreter self-test.
 *
 * This is the dxs360 integration point for emucore/a64. It runs one fixed
 * ARMv8-A program, assembled from known instruction encodings, on the
 * interpreter and checks the resulting architectural state:
 *
 *   - registers (MOVZ/MOVK/MOVN, ADD imm/shifted-register, loads)
 *   - flags (SUBS, via the full NZCV path)
 *   - memory   (STR/LDR 64-bit, STRB/LDRB, STR 32-bit + LDRSW)
 *   - branches (B.cond taken and not-taken, CBNZ loop, final PC)
 *
 * The program is position independent apart from the data pointer, which the
 * harness supplies in X28, so the same byte sequence is used everywhere.
 *
 * Encodings were produced with GNU as/objdump for aarch64 and the expected
 * results were captured from real AArch64 hardware (see emucore/a64/test).
 * The program words are written into guest memory byte-by-byte in
 * little-endian order, so this file is correct on a big-endian (PowerPC)
 * host as well.
 */
#include "a64.h"
#include <string.h>

#include "a64_test_entry.h"

/* ------------------------------------------------------------------ guest */

#define MEM_BASE   0x10000000u
#define MEM_SIZE   4096u
#define DATA_OFF   0x800u

#define PROG_LEN   21u
#define BRK_INDEX  20u
#define BRK_ADDR   (MEM_BASE + BRK_INDEX * 4u)

static uint8_t g_mem[MEM_SIZE];

static const uint32_t g_prog[PROG_LEN] = {
    0xd2824681u, 0xf2aacf01u, 0xd29fffe2u, 0x8b020023u, /* movz/movk/movz/add */
    0xf9000783u, 0xf9400784u, 0x52801e05u, 0x39000785u, /* str/ldr/movz/strb  */
    0x39400786u, 0x128001eau, 0xb900078au, 0xb9800787u, /* ldrb/movn/str/ldrsw*/
    0xeb03009fu, 0x54000040u, 0xd29bd5acu, 0xd2800028u, /* cmp/b.eq/movz/movz  */
    0xd2800069u, 0xf1000529u, 0xb5ffffe9u, 0x8b09010du, /* loop: subs/cbnz/add */
    0xd4202460u,                                        /* brk #0x123         */
};

static int mem_read(void *ctx, uint64_t addr, void *buf, size_t len) {
    (void)ctx;
    if (addr < MEM_BASE || addr > MEM_BASE + MEM_SIZE) return 0;
    if (len > MEM_SIZE - (size_t)(addr - MEM_BASE)) return 0;
    memcpy(buf, g_mem + (size_t)(addr - MEM_BASE), len);
    return 1;
}

static int mem_write(void *ctx, uint64_t addr, void *buf, size_t len) {
    (void)ctx;
    if (addr < MEM_BASE || addr > MEM_BASE + MEM_SIZE) return 0;
    if (len > MEM_SIZE - (size_t)(addr - MEM_BASE)) return 0;
    memcpy(g_mem + (size_t)(addr - MEM_BASE), buf, len);
    return 1;
}

/* ------------------------------------------------------------- reporting */

static int  g_fails;
static char g_report[192];
static size_t g_rlen;

static void fail_mark(const char *name) {
    size_t n = strlen(name);
    ++g_fails;
    if (g_rlen && g_rlen + 1 < sizeof g_report) g_report[g_rlen++] = ',';
    if (g_rlen + n < sizeof g_report) {
        memcpy(g_report + g_rlen, name, n);
        g_rlen += n;
        g_report[g_rlen] = '\0';
    }
}

#define CHECK(name, cond) do { if (!(cond)) fail_mark(name); } while (0)
#define CHECK_EQ(name, got, want) CHECK(name, (uint64_t)(got) == (uint64_t)(want))

/* ---------------------------------------------------------------- test run */

int eden_a64_selftest(void) {
    A64 cpu;
    A64Halt h = A64_OK;
    unsigned i;

    g_fails = 0;
    g_rlen = 0;
    g_report[0] = '\0';

    /* Load the program into guest memory, little-endian, host-independent. */
    memset(g_mem, 0, sizeof g_mem);
    for (i = 0; i < PROG_LEN; ++i) {
        uint32_t w = g_prog[i];
        g_mem[i * 4 + 0] = (uint8_t)(w & 0xFF);
        g_mem[i * 4 + 1] = (uint8_t)((w >> 8) & 0xFF);
        g_mem[i * 4 + 2] = (uint8_t)((w >> 16) & 0xFF);
        g_mem[i * 4 + 3] = (uint8_t)((w >> 24) & 0xFF);
    }

    A64_Init(&cpu, mem_read, mem_write, 0);
    A64_Reset(&cpu, MEM_BASE, 1);
    A64_SetX(&cpu, 28, MEM_BASE + DATA_OFF); /* X28 = data pointer */

    for (i = 0; i < 64 && h == A64_OK; ++i) h = A64_Step(&cpu);

    /* termination */
    CHECK_EQ("halt", h, A64_HALT_BRK);
    CHECK_EQ("brk_imm", cpu.svc_imm, 0x123u);
    CHECK_EQ("final_pc", cpu.pc, (uint64_t)(BRK_ADDR + 4u));

    /* registers: move-wide + arithmetic */
    CHECK_EQ("x1",  A64_GetX(&cpu, 1),  0x0000000056781234ull);
    CHECK_EQ("x2",  A64_GetX(&cpu, 2),  0x000000000000ffffull);
    CHECK_EQ("x3",  A64_GetX(&cpu, 3),  0x0000000056791233ull);
    CHECK_EQ("x4",  A64_GetX(&cpu, 4),  0x0000000056791233ull);
    CHECK_EQ("x5",  A64_GetX(&cpu, 5),  0x00000000000000f0ull);
    CHECK_EQ("x6",  A64_GetX(&cpu, 6),  0x00000000000000f0ull);
    CHECK_EQ("x7",  A64_GetX(&cpu, 7),  0xfffffffffffffff0ull);
    CHECK_EQ("x8",  A64_GetX(&cpu, 8),  0x0000000000000001ull);
    CHECK_EQ("x9",  A64_GetX(&cpu, 9),  0x0000000000000000ull);
    CHECK_EQ("x10", A64_GetX(&cpu, 10), 0x00000000fffffff0ull);
    CHECK_EQ("x11", A64_GetX(&cpu, 11), 0x0000000000000000ull);
    /* x12 proves the B.EQ was taken (the MOVZ #0xDEAD was skipped). */
    CHECK_EQ("x12", A64_GetX(&cpu, 12), 0x0000000000000000ull);
    /* x13 = x8 + x9 after the CBNZ loop terminated. */
    CHECK_EQ("x13", A64_GetX(&cpu, 13), 0x0000000000000001ull);

    /* flags: last SUBS was 1 - 1 = 0 -> Z=1, C=1, N=0, V=0 */
    CHECK_EQ("nzcv", cpu.nzcv, 0x60000000u);

    /* memory: store/load round trips. Guest memory is little-endian; rebuild
     * values byte-wise so the check is valid on big-endian hosts too. */
    CHECK_EQ("mem8",  g_mem[DATA_OFF + 1], 0xf0u);
    {
        uint32_t v32 = 0;
        uint64_t v64 = 0;
        unsigned i;
        for (i = 0; i < 4; ++i) v32 |= (uint32_t)g_mem[DATA_OFF + 4 + i] << (8 * i);
        for (i = 0; i < 8; ++i) v64 |= (uint64_t)g_mem[DATA_OFF + 8 + i] << (8 * i);
        CHECK_EQ("mem32", v32, 0xfffffff0u);
        CHECK_EQ("mem64", v64, 0x0000000056791233ull);
    }

    return g_fails;
}

const char *eden_a64_selftest_report(void) {
    return g_report;
}
