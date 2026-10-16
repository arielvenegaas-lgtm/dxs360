/* test_a64.c - unit tests for the portable AArch64 interpreter.
 *
 * Two layers:
 *   1. Portable checks: hand-verified encodings with known results. Run
 *      everywhere (including x86_64 CI).
 *   2. Differential checks (aarch64 hosts only): every listed encoding is run
 *      natively and in the interpreter from identical random state; results
 *      (registers, SP, NZCV, memory) must match exactly.
 *
 * Reserved registers in the native harness: X16, X17, X30 (not compared).
 */

#include "../a64.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>

#define REGION_SIZE (1u << 20)
#define CODE_OFF    0x00000u
#define DATA_OFF    0x04000u
#define DATA_SIZE   0x04000u

static uint8_t *g_region;   /* RWX guest memory, also holds test code */

static int mem_read(void *ctx, uint64_t addr, void *buf, size_t len) {
    (void)ctx;
    uint64_t base = (uint64_t)(uintptr_t)g_region;
    if (addr < base || addr + len > base + REGION_SIZE) return 0;
    memcpy(buf, (void *)(uintptr_t)addr, len);
    return 1;
}

static int mem_write(void *ctx, uint64_t addr, void *buf, size_t len) {
    (void)ctx;
    uint64_t base = (uint64_t)(uintptr_t)g_region;
    if (addr < base || addr + len > base + REGION_SIZE) return 0;
    memcpy((void *)(uintptr_t)addr, buf, len);
    return 1;
}

static int g_checks = 0, g_fails = 0;

#define CHECK(cond, msg) do {                                   \
    ++g_checks;                                                 \
    if (!(cond)) { printf("  FAIL: %s\n", (msg)); ++g_fails; }  \
} while (0)

/* ------------------------------------------------- portable unit checks -- */

static void put_code(const uint32_t *code, size_t n) {
    memcpy(g_region + CODE_OFF, code, n * 4);
    __builtin___clear_cache((char *)(g_region + CODE_OFF),
                            (char *)(g_region + CODE_OFF) + n * 4);
}

static A64 fresh(void) {
    A64 c;
    A64_Init(&c, mem_read, mem_write, NULL);
    A64_Reset(&c, (uint64_t)(uintptr_t)(g_region + CODE_OFF), 1);
    c.sp = (uint64_t)(uintptr_t)(g_region + DATA_OFF + 0x800);
    return c;
}

static void test_portable(void) {
    printf("portable encoding checks\n");
    A64 c;
    const uint32_t *code = (const uint32_t *)(const void *)(g_region + CODE_OFF);
    (void)code;

    /* mov x0,#5 ; add x0,x0,#3 -> 8 */
    { uint32_t p[] = {0xd28000a0u, 0x91000c00u};
      put_code(p, 2); c = fresh(); A64_Run(&c, 2);
      CHECK(c.halt == A64_OK, "add halt");
      CHECK(c.x[0] == 8, "mov+add -> 8"); }

    /* movz x5,#0x1234,lsl16 ; movk x5,#0xabcd -> 0x1234abcd */
    { uint32_t p[] = {0xd2a24685u, 0xf29579a5u};
      put_code(p, 2); c = fresh(); A64_Run(&c, 2);
      CHECK(c.x[5] == 0x1234abcdu, "movz+movk"); }

    /* subs x1,x0,#1 with x0=5 -> 4, and NZCV Z=0 N=0 C=1 */
    { uint32_t p[] = {0xf1000401u};
      put_code(p, 1); c = fresh(); c.x[0] = 5; A64_Step(&c);
      CHECK(c.x[1] == 4, "subs imm");
      CHECK((c.nzcv & A64_NZCV_C) != 0, "subs imm carry"); }

    /* orr x2,x0,#0xf0 with x0=0x0f -> 0xff */
    { uint32_t p[] = {0xb27c0c02u};
      put_code(p, 1); c = fresh(); c.x[0] = 0x0f; A64_Step(&c);
      CHECK(c.x[2] == 0xff, "orr logical imm"); }

    /* and x3,x2,#0xff with x2=0x1234 -> 0x34 */
    { uint32_t p[] = {0x92401c43u};
      put_code(p, 1); c = fresh(); c.x[2] = 0x1234; A64_Step(&c);
      CHECK(c.x[3] == 0x34, "and logical imm"); }

    /* eor x4,x3,x2 with x3=0xf0f0 x2=0x0ff0 -> 0xff00 */
    { uint32_t p[] = {0xca020064u};
      put_code(p, 1); c = fresh(); c.x[3] = 0xf0f0; c.x[2] = 0x0ff0; A64_Step(&c);
      CHECK(c.x[4] == 0xff00, "eor"); }

    /* lsl x6,x0,#4 with x0=0x21 -> 0x210 */
    { uint32_t p[] = {0xd37cec06u};
      put_code(p, 1); c = fresh(); c.x[0] = 0x21; A64_Step(&c);
      CHECK(c.x[6] == 0x210, "lsl imm"); }

    /* lsr x7,x6,#2 with x6=0x210 -> 0x84 */
    { uint32_t p[] = {0xd342fcc7u};
      put_code(p, 1); c = fresh(); c.x[6] = 0x210; A64_Step(&c);
      CHECK(c.x[7] == 0x84, "lsr imm"); }

    /* asr x8,x7,#1 with x7=0xfffffffffffffff0 -> 0xfffffffffffffff8 */
    { uint32_t p[] = {0x9341fce8u};
      put_code(p, 1); c = fresh();
      c.x[7] = 0xfffffffffffffff0ull; A64_Step(&c);
      CHECK(c.x[8] == 0xfffffffffffffff8ull, "asr imm"); }

    /* madd x9,x0,x1,x2 with 3,4,5 -> 17 */
    { uint32_t p[] = {0x9b010809u};
      put_code(p, 1); c = fresh();
      c.x[0] = 3; c.x[1] = 4; c.x[2] = 5; A64_Step(&c);
      CHECK(c.x[9] == 17, "madd"); }

    /* msub x10,x0,x1,x2 with 3,4,5 -> 5-12 = -7 */
    { uint32_t p[] = {0x9b01880au};
      put_code(p, 1); c = fresh();
      c.x[0] = 3; c.x[1] = 4; c.x[2] = 5; A64_Step(&c);
      CHECK(c.x[10] == (uint64_t)-7, "msub"); }

    /* udiv x11,x9,x1 with 100,7 -> 14 */
    { uint32_t p[] = {0x9ac1092bu};
      put_code(p, 1); c = fresh();
      c.x[9] = 100; c.x[1] = 7; A64_Step(&c);
      CHECK(c.x[11] == 14, "udiv"); }

    /* sdiv x12,x9,x1 with -100,7 -> -14 */
    { uint32_t p[] = {0x9ac10d2cu};
      put_code(p, 1); c = fresh();
      c.x[9] = (uint64_t)-100; c.x[1] = 7; A64_Step(&c);
      CHECK(c.x[12] == (uint64_t)-14, "sdiv"); }

    /* cmp x0,#5 ; cset x14,eq with x0=5 -> 1 */
    { uint32_t p[] = {0xf100141fu, 0x9a9f17eeu};
      put_code(p, 2); c = fresh(); c.x[0] = 5; A64_Run(&c, 2);
      CHECK(c.x[14] == 1, "cmp+cset eq"); }

    /* rev x18,x0 with 0x0102030405060708 -> 0x0807060504030201 */
    { uint32_t p[] = {0xdac00c12u};
      put_code(p, 1); c = fresh();
      c.x[0] = 0x0102030405060708ull; A64_Step(&c);
      CHECK(c.x[18] == 0x0807060504030201ull, "rev"); }

    /* clz x19,x0 with 0x0f -> 60 */
    { uint32_t p[] = {0xdac01013u};
      put_code(p, 1); c = fresh(); c.x[0] = 0x0f; A64_Step(&c);
      CHECK(c.x[19] == 60, "clz"); }

    /* rbit x20,x0 with 0x1 -> 0x8000000000000000 */
    { uint32_t p[] = {0xdac00014u};
      put_code(p, 1); c = fresh(); c.x[0] = 1; A64_Step(&c);
      CHECK(c.x[20] == 0x8000000000000000ull, "rbit"); }

    /* str x0,[sp] ; ldr x21,[sp] -> roundtrip 0xdeadbeefcafebabe */
    { uint32_t p[] = {0xf90003e0u, 0xf94003f5u};
      put_code(p, 2); c = fresh();
      c.x[0] = 0xdeadbeefcafebabeull; A64_Run(&c, 2);
      CHECK(c.x[21] == 0xdeadbeefcafebabeull, "str/ldr roundtrip"); }

    /* strb w0,[sp,#4] ; ldrb w1,[sp,#4] with 0x1ff -> 0xff */
    { uint32_t p[] = {0x390013e0u, 0x394013e1u};
      put_code(p, 2); c = fresh(); c.x[0] = 0x1ff; A64_Run(&c, 2);
      CHECK(c.x[1] == 0xff, "strb/ldrb"); }

    /* stp/ldp pre/post index */
    { uint32_t p[] = {0xa9bf0be1u, 0xa8c113e3u};
      put_code(p, 2); c = fresh();
      c.x[1] = 0x1111; c.x[2] = 0x2222;
      uint64_t sp0 = c.sp;
      A64_Run(&c, 2);
      CHECK(c.x[3] == 0x1111 && c.x[4] == 0x2222, "stp/ldp values");
      CHECK(c.sp == sp0, "stp/ldp sp restored"); }

    /* adc x13,x0,x1 with x0=1,x1=2,C=1 -> 4 */
    { uint32_t p[] = {0x9a01000du};
      put_code(p, 1); c = fresh();
      c.x[0] = 1; c.x[1] = 2; c.nzcv = A64_NZCV_C; A64_Step(&c);
      CHECK(c.x[13] == 4, "adc"); }

    /* extr x4,x5,x6,#8 with x5=0x1122334455667788, x6=0x99aabbccddeeff00
       result = (x5 << 56) | (x6 >> 8) = 0x8899aabbccddeeff */
    { uint32_t p[] = {0x93c620a4u};
      put_code(p, 1); c = fresh();
      c.x[5] = 0x1122334455667788ull; c.x[6] = 0x99aabbccddeeff00ull;
      A64_Step(&c);
      CHECK(c.x[4] == 0x8899aabbccddeeffull, "extr"); }

    /* mul x13,x0,x1 with 0x100000000,3 -> 0x300000000 */
    { uint32_t p[] = {0x9b017c0du};
      put_code(p, 1); c = fresh();
      c.x[0] = 0x100000000ull; c.x[1] = 3; A64_Step(&c);
      CHECK(c.x[13] == 0x300000000ull, "mul 64"); }

    /* neg x23,x0 with 7 -> -7 */
    { uint32_t p[] = {0xcb0003f7u};
      put_code(p, 1); c = fresh(); c.x[0] = 7; A64_Step(&c);
      CHECK(c.x[23] == (uint64_t)-7, "neg"); }

    /* smulh x9,x0,x1 with 0x1000000000000000 * 4 -> 0x4000000000000000 (fits, high=0) */
    { uint32_t p[] = {0x9b417c09u};
      put_code(p, 1); c = fresh();
      c.x[0] = 0x1000000000000000ull; c.x[1] = 4; A64_Step(&c);
      CHECK(c.x[9] == 0, "smulh"); }

    /* rev16 x13,x0 with 0x0102 -> 0x0201 */
    { uint32_t p[] = {0xdac0040du};
      put_code(p, 1); c = fresh(); c.x[0] = 0x0102; A64_Step(&c);
      CHECK(c.x[13] == 0x0201, "rev16"); }

    /* sxtb x18,w0 with 0x80 -> 0xffffffffffffff80 */
    { uint32_t p[] = {0x93401c12u};
      put_code(p, 1); c = fresh(); c.x[0] = 0x80; A64_Step(&c);
      CHECK(c.x[18] == 0xffffffffffffff80ull, "sxtb"); }

    /* uxtb w21,w0 with 0x1ff -> 0xff */
    { uint32_t p[] = {0x53001c15u};
      put_code(p, 1); c = fresh(); c.x[0] = 0x1ff; A64_Step(&c);
      CHECK(c.x[21] == 0xff, "uxtb"); }

    /* adds x8,x0,x1 with 0xffffffffffffffff + 1 -> 0, C=1 Z=1 */
    { uint32_t p[] = {0xab010008u};
      put_code(p, 1); c = fresh();
      c.x[0] = ~0ull; c.x[1] = 1; A64_Step(&c);
      CHECK(c.x[8] == 0, "adds result");
      CHECK((c.nzcv & A64_NZCV_C) && (c.nzcv & A64_NZCV_Z), "adds flags"); }

    /* csel x15,x0,x1,eq with Z set -> x0 */
    { uint32_t p[] = {0x9a81000fu};
      put_code(p, 1); c = fresh();
      c.x[0] = 0x1111; c.x[1] = 0x2222; c.nzcv = A64_NZCV_Z; A64_Step(&c);
      CHECK(c.x[15] == 0x1111, "csel taken"); }

    /* ubfx x29,x0,#4,#8 with 0xff0 -> 0xff */
    { uint32_t p[] = {0xd3442c1du};
      put_code(p, 1); c = fresh(); c.x[0] = 0xff0; A64_Step(&c);
      CHECK(c.x[29] == 0xff, "ubfx"); }

    /* ubfx w-reg version ubfiz w2,w0,#4,#8 with 0xab -> 0xab0 */
    { uint32_t p[] = {0x531c1c02u};
      put_code(p, 1); c = fresh(); c.x[0] = 0xab; A64_Step(&c);
      CHECK(c.x[2] == 0xab0, "ubfiz w"); }

    /* bfi x2,x3,#4,#8 with x2=0xffff0000, x3=0xab -> bits[11:4]=0xab, rest kept
       -> (0xab << 4) | (0xffff0000 & ~0xff0) = 0xab0 | 0xffff0000 = 0xffff0ab0 */
    { uint32_t p[] = {0xb37c1c62u};
      put_code(p, 1); c = fresh();
      c.x[2] = 0xffff0000ull; c.x[3] = 0xab; A64_Step(&c);
      CHECK(c.x[2] == 0xffff0ab0ull, "bfi"); }

    /* branches: b.cond */
    { uint32_t p[] = {0x54000040u /* b.eq +8 */, 0xd280001fu /* nop? */,
                      0xd2800020u /* mov x0,#1 */, 0xd2800040u /* mov x0,#2 */};
      put_code(p, 4); c = fresh(); c.nzcv = A64_NZCV_Z;
      A64_Step(&c);
      CHECK(c.pc == (uint64_t)(uintptr_t)(g_region + CODE_OFF) + 8, "b.eq taken pc");
      A64_Run(&c, 1);
      CHECK(c.x[0] == 1, "b.eq taken exec"); }

    /* cbz */
    { uint32_t p[] = {0xb4000040u /* cbz x0,+8 */, 0xd2800020u, 0xd2800040u};
      put_code(p, 3); c = fresh(); c.x[0] = 0;
      A64_Step(&c);
      CHECK(c.pc == (uint64_t)(uintptr_t)(g_region + CODE_OFF) + 8, "cbz taken"); }

    /* tbz w0,#2,+8 with bit2 clear -> taken */
    { uint32_t p[] = {0x36100040u /* tbz w0,#2,+8 */, 0xd2800020u, 0xd2800040u};
      put_code(p, 3); c = fresh(); c.x[0] = 0;
      A64_Step(&c);
      CHECK(c.pc == (uint64_t)(uintptr_t)(g_region + CODE_OFF) + 8, "tbz taken"); }

    /* bl sets x30; then ret jumps back */
    { uint32_t p[] = {0x94000002u /* bl +8 */, 0xd65f03c0u /* ret (not reached) */,
                      0xd65f03c0u /* ret target... actually lands here */};
      put_code(p, 3); c = fresh();
      A64_Step(&c);
      CHECK(c.x[30] == (uint64_t)(uintptr_t)(g_region + CODE_OFF) + 4, "bl lr");
      CHECK(c.pc == (uint64_t)(uintptr_t)(g_region + CODE_OFF) + 8, "bl pc"); }

    /* svc halts with immediate */
    { uint32_t p[] = {0xd4000001u}; /* svc #0 */
      put_code(p, 1); c = fresh(); A64_Step(&c);
      CHECK(c.halt == A64_HALT_SVC, "svc halt"); }

    /* brk halts */
    { uint32_t p[] = {0xd4200000u}; /* brk #0 */
      put_code(p, 1); c = fresh(); A64_Step(&c);
      CHECK(c.halt == A64_HALT_BRK, "brk halt"); }

    /* unimplemented FP instruction must halt, not silently succeed */
    { uint32_t p[] = {0x1e202008u}; /* fadd s0,s0,s0 (FP) */
      put_code(p, 1); c = fresh(); A64_Step(&c);
      CHECK(c.halt == A64_HALT_UNIMPLEMENTED, "fp halts unimplemented"); }

    /* memory fault is reported */
    { uint32_t p[] = {0xf90003e0u};
      put_code(p, 1); c = fresh();
      c.sp = 0x1000; /* outside region */
      A64_Step(&c);
      CHECK(c.halt == A64_HALT_MEMFAULT, "mem fault reported"); }
}

/* --------------------------------------------------- differential (a64) -- */

#if defined(__aarch64__)

typedef struct { uint64_t x[31]; uint64_t sp; uint64_t nzcv; } NativeState;

extern void native_exec(NativeState *st, const void *code);

static uint64_t g_rng = 0x123456789abcdef0ull;
static uint64_t rnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return g_rng;
}

/* Straight-line encodings (no branches), from the native assembler. */
static const uint32_t kInsns[] = {
    0xd28000a0u, 0x91000c00u, 0xd1000401u, 0xb27c0c02u, 0x92401c43u,
    0xca020064u, 0xd2a24685u, 0xf29579a5u, 0xd37cec06u, 0xd342fcc7u,
    0x9341fce8u, 0x9b010809u, 0x9b01880au, 0x9ac1092bu, 0x9ac10d2cu,
    0x9b017c0du, 0xf100141fu, 0x9a9f17eeu, 0x9a81000fu, 0xdac00c12u,
    0xdac01013u, 0xdac00014u, 0xf90003e0u, 0xf94003f5u, 0x8b010816u,
    0x10000017u, 0x4b010018u, 0x6b01001fu, 0x90000019u, 0xca1c037au,
    0xd3442c1du, 0x93442c20u, 0xb37c1c62u, 0x93c620a4u, 0xfa410000u,
    0x390013e0u, 0x394013e1u, 0x398013e2u, 0x79000fe3u, 0x79400fe4u,
    0x79800fe5u, 0xb9400be6u, 0xb9000fe6u, 0xa9bf0be1u, 0xa8c113e3u,
    0xf9000be5u, 0xf9400be6u, 0xf84183e7u, 0xf80183e8u, 0xf82a6be9u,
    0xf86c7bebu, 0x9a01000du, 0xda01000eu, 0xda81100fu, 0xd3483c02u,
    0x93483c03u, 0xb3483c04u, 0xd37c1c05u, 0x9ac12406u, 0x9ac12c07u,
    0x9ac12808u, 0x9b417c09u, 0x9bc17c0au, 0x9b21080bu, 0x9ba1080cu,
    0xdac0040du, 0xdac0080eu, 0xdac0140fu, 0x93401c12u, 0x93403c13u,
    0x93407c14u, 0x53001c15u, 0x53003c16u, 0xcb0003f7u, 0xaa2003f8u,
    0xaa0003f9u, 0xda80141au, 0x9a80041bu, 0xda81b41cu, 0x531c1c02u,
    0x531d7003u, 0x0a010004u, 0x2a010805u, 0x4a810406u, 0x4b410807u,
    0xab010008u, 0x6b010009u, 0x2a2003eau, 0x1a81c00bu, 0x9a81d40cu,
    0x9b217c0du, 0x9ba17c0eu, 0x9b21fc0fu, 0x9b417c1du,
};

static void test_differential(void) {
    printf("differential checks vs native aarch64 (%zu encodings x 200)\n",
           sizeof(kInsns) / sizeof(kInsns[0]));

    uint8_t *base    = g_region + CODE_OFF;
    uint8_t *data    = g_region + DATA_OFF;
    uint8_t *snapshot = malloc(DATA_SIZE);
    uint8_t *nat_dat  = malloc(DATA_SIZE);
    size_t mismatches = 0;

    for (size_t k = 0; k < sizeof(kInsns) / sizeof(kInsns[0]); ++k) {
        uint32_t insn = kInsns[k];
        for (int trial = 0; trial < 200; ++trial) {
            NativeState in, out;
            memset(&in, 0, sizeof(in));
            for (int i = 0; i < 31; ++i) in.x[i] = rnd();
            in.sp = (uint64_t)(uintptr_t)(data + 0x800);

            /* keep memory operands inside the region */
            in.x[10] = rnd() & 0x3f0;
            in.x[12] = rnd() & 0x1ff;
            in.nzcv = (uint32_t)(rnd() & 0xf0000000u);

            for (size_t b = 0; b < DATA_SIZE; ++b) data[b] = (uint8_t)rnd();
            memcpy(snapshot, data, DATA_SIZE);

            /* native run */
            uint32_t *code = (uint32_t *)(void *)base;
            code[0] = insn;
            code[1] = 0xd65f03c0u; /* ret */
            __builtin___clear_cache((char *)base, (char *)base + 8);
            out = in;
            native_exec(&out, base);
            memcpy(nat_dat, data, DATA_SIZE);

            /* interpreter run */
            memcpy(data, snapshot, DATA_SIZE);
            A64 c; A64_Init(&c, mem_read, mem_write, NULL);
            A64_Reset(&c, (uint64_t)(uintptr_t)base, 1);
            memcpy(c.x, in.x, sizeof(in.x));
            c.sp = in.sp;
            c.nzcv = in.nzcv;
            A64_Step(&c);

            int mismatch = 0;
            for (int i = 0; i < 31 && !mismatch; ++i) {
                if (i == 16 || i == 17 || i == 30) continue;
                if (c.x[i] != out.x[i]) {
                    if (mismatches < 20)
                        printf("  reg x%d insn=0x%08x got=%016llx want=%016llx\n",
                               i, insn, (unsigned long long)c.x[i],
                               (unsigned long long)out.x[i]);
                    mismatch = 1;
                }
            }
            if (!mismatch && (uint32_t)c.nzcv != (uint32_t)out.nzcv) {
                if (mismatches < 20)
                    printf("  nzcv insn=0x%08x got=%08x want=%08x\n", insn,
                           c.nzcv, (uint32_t)out.nzcv);
                mismatch = 1;
            }
            if (!mismatch && c.sp != out.sp) {
                if (mismatches < 20)
                    printf("  sp insn=0x%08x got=%016llx want=%016llx\n", insn,
                           (unsigned long long)c.sp, (unsigned long long)out.sp);
                mismatch = 1;
            }
            if (!mismatch && c.halt != A64_OK) {
                if (mismatches < 20)
                    printf("  halt insn=0x%08x = %d\n", insn, (int)c.halt);
                mismatch = 1;
            }
            if (!mismatch && memcmp(data, nat_dat, DATA_SIZE) != 0) {
                if (mismatches < 20)
                    printf("  mem insn=0x%08x differs\n", insn);
                mismatch = 1;
            }
            if (mismatch) ++mismatches;
        }
        if (mismatches) {
            printf("  stop after encoding 0x%08x (%zu mismatches)\n", insn, mismatches);
            break;
        }
    }

    CHECK(mismatches == 0, "differential: all encodings match native");
    free(snapshot); free(nat_dat);
}
#endif

int main(void) {
    /* mmap an RWX region so the native oracle can run generated code. */
    g_region = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_region == MAP_FAILED) { perror("mmap"); return 2; }

    test_portable();

#if defined(__aarch64__)
    test_differential();
#else
    printf("differential checks skipped (host is not aarch64)\n");
#endif

    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
