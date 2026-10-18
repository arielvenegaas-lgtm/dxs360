/* a64.c: portable AArch64 interpreter. See a64.h. */
#include "a64.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---------------------------------------------------------------- helpers */

static uint32_t Field(uint32_t v, int hi, int lo) {
    int n = hi - lo + 1;
    if (n >= 32) return v >> lo;
    return (v >> lo) & ((1u << n) - 1);
}

static uint64_t Ones(int n) {
    if (n <= 0) return 0;
    if (n >= 64) return ~0ull;
    return (1ull << n) - 1;
}

static uint64_t SignExtend(uint64_t v, int bits) {
    if (bits <= 0) return 0;
    if (bits >= 64) return v;
    uint64_t m = 1ull << (bits - 1);
    return (v ^ m) - m;
}

static uint64_t Ror64(uint64_t v, unsigned n) {
    n &= 63;
    if (!n) return v;
    return (v >> n) | (v << (64 - n));
}

static uint32_t Ror32(uint32_t v, unsigned n) {
    n &= 31;
    if (!n) return v;
    return (v >> n) | (v << (32 - n));
}

static unsigned Clz64(uint64_t v) {
    unsigned n = 0;
    if (!v) return 64;
    if (!(v >> 32)) { n += 32; v <<= 32; }
    if (!(v >> 48)) { n += 16; v <<= 16; }
    if (!(v >> 56)) { n += 8;  v <<= 8;  }
    if (!(v >> 60)) { n += 4;  v <<= 4;  }
    if (!(v >> 62)) { n += 2;  v <<= 2;  }
    if (!(v >> 63)) { n += 1; }
    return n;
}
static unsigned Clz32(uint32_t v) { return v ? (Clz64(v) - 32) : 32; }

/* Count leading sign bits (after the sign bit). */
static uint32_t ClsCount(uint64_t x, int regsize) {
    uint64_t sign = (x >> (regsize - 1)) & 1;
    uint32_t n = 0;
    for (int i = regsize - 2; i >= 0; --i) {
        if (((x >> i) & 1) != sign) break;
        ++n;
    }
    return n;
}

static uint64_t RevBytes(uint64_t v, int nbytes) {
    uint64_t r = 0;
    for (int i = 0; i < nbytes; ++i) {
        r = (r << 8) | (v & 0xFF);
        v >>= 8;
    }
    return r;
}

static uint64_t BitReverse(uint64_t v, int bits) {
    uint64_t r = 0;
    for (int i = 0; i < bits; ++i) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

static uint64_t MulHi64(uint64_t a, uint64_t b) {
    uint64_t al = a & 0xFFFFFFFF, ah = a >> 32;
    uint64_t bl = b & 0xFFFFFFFF, bh = b >> 32;
    uint64_t p0 = al * bl;
    uint64_t p1 = al * bh;
    uint64_t p2 = ah * bl;
    uint64_t p3 = ah * bh;
    uint64_t mid = (p0 >> 32) + (p1 & 0xFFFFFFFF) + (p2 & 0xFFFFFFFF);
    return p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
}

static uint64_t ShiftVal(uint64_t v, unsigned type, unsigned amount, int regsize) {
    if (regsize == 32) v &= 0xFFFFFFFFu;
    if (type == 0) { /* LSL */
        if (amount >= (unsigned)regsize) return 0;
        if (regsize == 32) return (v << amount) & 0xFFFFFFFFu;
        return v << amount;
    } else if (type == 1) { /* LSR */
        if (amount >= (unsigned)regsize) return 0;
        return v >> amount;
    } else if (type == 2) { /* ASR */
        if (amount >= (unsigned)regsize) {
            if (regsize == 32) return (v & 0x80000000u) ? 0xFFFFFFFFu : 0;
            return (v & 0x8000000000000000ull) ? ~0ull : 0;
        }
        if (regsize == 32) {
            if (!amount) return (uint64_t)(int32_t)(uint32_t)v;
            return (uint64_t)(int32_t)(((uint32_t)v >> amount) |
                   ((v & 0x80000000u) ? (0xFFFFFFFFu << (32 - amount)) : 0));
        }
        return (uint64_t)((int64_t)v >> amount);
    } else { /* ROR */
        if (regsize == 32) return Ror32((uint32_t)v, amount);
        return Ror64(v, amount);
    }
}

static uint64_t ExtendVal(uint64_t v, unsigned option) {
    switch (option) {
        case 0: return v & 0xFF;
        case 1: return v & 0xFFFF;
        case 2: return v & 0xFFFFFFFFu;
        case 3: return v;
        case 4: return (uint64_t)(int64_t)(int8_t)(v & 0xFF);
        case 5: return (uint64_t)(int64_t)(int16_t)(v & 0xFFFF);
        case 6: return (uint64_t)(int64_t)(int32_t)(v & 0xFFFFFFFF);
        default: return v;
    }
}

/* ---------------------------------------------------------- register I/O */

uint64_t A64_GetX(const A64 *c, unsigned n) { return n == 31 ? 0 : c->x[n]; }
void A64_SetX(A64 *c, unsigned n, uint64_t v) { if (n != 31) c->x[n] = v; }

static uint64_t ReadX(const A64 *c, unsigned n) { return n == 31 ? 0 : c->x[n]; }
static uint32_t ReadW(const A64 *c, unsigned n) { return (uint32_t)(n == 31 ? 0 : c->x[n]); }
static void WriteX(A64 *c, unsigned n, uint64_t v) { if (n != 31) c->x[n] = v; }
static void WriteW(A64 *c, unsigned n, uint32_t v) { if (n != 31) c->x[n] = v; }

static uint64_t ReadXSp(const A64 *c, unsigned n) { return n == 31 ? c->sp : c->x[n]; }
static void WriteXSp(A64 *c, unsigned n, uint64_t v) { if (n == 31) c->sp = v; else c->x[n] = v; }
static void WriteWSp(A64 *c, unsigned n, uint32_t v) { if (n == 31) c->sp = v; else c->x[n] = v & 0xFFFFFFFF; }

/* ------------------------------------------------------------- conditions */

static int CondCheck(uint32_t nzcv, unsigned cond) {
    if (cond >= 14) return 1;
    int n = (nzcv & A64_NZCV_N) != 0;
    int z = (nzcv & A64_NZCV_Z) != 0;
    int c = (nzcv & A64_NZCV_C) != 0;
    int v = (nzcv & A64_NZCV_V) != 0;
    int base;
    switch (cond >> 1) {
        case 0: base = z; break;
        case 1: base = c; break;
        case 2: base = n; break;
        case 3: base = v; break;
        case 4: base = c && !z; break;
        case 5: base = n == v; break;
        case 6: base = n == v && !z; break;
        default: base = 1; break;
    }
    return base ^ (int)(cond & 1);
}

static uint64_t AddWithCarry(A64 *c, uint64_t x, uint64_t y, uint32_t carry_in, int regsize) {
    uint64_t mask = (regsize == 32) ? 0xFFFFFFFFull : ~0ull;
    uint64_t xm = x & mask, ym = y & mask;

    uint64_t sum = xm + ym;
    uint32_t carry;
    if (regsize == 32) {
        carry = (uint32_t)((sum >> 32) & 1);
    } else {
        carry = (sum < xm) ? 1u : 0u;
    }
    sum &= mask;

    uint64_t s = sum + carry_in;
    if (regsize == 32) {
        if (s >> 32) carry = 1;
    } else {
        if (s < sum) carry = 1;
    }
    s &= mask;

    uint64_t signbit = (regsize == 32) ? 0x80000000ull : 0x8000000000000000ull;
    uint32_t n = (s & signbit) ? 1u : 0u;
    uint32_t z = (s == 0) ? 1u : 0u;
    uint32_t sa = (xm & signbit) ? 1u : 0u;
    uint32_t sb = (ym & signbit) ? 1u : 0u;
    uint32_t sr = (s & signbit) ? 1u : 0u;
    uint32_t v = (sa == sb && sr != sa) ? 1u : 0u;
    c->nzcv = (n ? A64_NZCV_N : 0) | (z ? A64_NZCV_Z : 0) |
              (carry ? A64_NZCV_C : 0) | (v ? A64_NZCV_V : 0);
    return s;
}

static void SetLogicFlags(A64 *c, uint64_t result, int regsize) {
    uint64_t mask = (regsize == 32) ? 0xFFFFFFFFull : ~0ull;
    result &= mask;
    c->nzcv = ((result & (mask & ~(mask >> 1))) ? A64_NZCV_N : 0) |
              (result == 0 ? A64_NZCV_Z : 0);
}

/* Decode a logical-immediate bitmask (ARM "DecodeBitMasks", immediate=TRUE). */
static uint64_t RorWithin(uint64_t v, uint32_t r, int size) {
    uint64_t mask = Ones(size);
    v &= mask;
    if (r == 0) return v;
    return ((v >> r) | (v << (size - r))) & mask;
}

static int DecodeLogImm(uint32_t N, uint32_t imms, uint32_t immr, int regsize, uint64_t *out) {
    uint32_t x = (N << 6) | ((~imms) & 0x3F);
    if (!x) return 0;
    int len = 31 - (int)Clz32(x);
    if (len < 1) return 0;
    int size = 1 << len;
    if (size < 2 || size > regsize) return 0;
    if (regsize == 32 && N != 0) return 0;
    uint32_t levels = (uint32_t)(size - 1);
    uint32_t S = imms & levels;
    uint32_t R = immr & levels;
    uint64_t welem = Ones((int)(S + 1));
    uint64_t elem = RorWithin(welem, R, size);
    uint64_t w = 0;
    for (int i = 0; i < 64; i += size) w |= elem << i;
    if (regsize == 32) w &= 0xFFFFFFFFu;
    *out = w;
    return 1;
}

/* ------------------------------------------------------------- memory I/O */

static int MemRead(A64 *c, uint64_t addr, void *buf, size_t len) {
    if (c->read == NULL || !c->read(c->memctx, addr, buf, len)) {
        c->halt = A64_HALT_MEMFAULT;
        c->fault_addr = addr;
        c->fault_size = len;
        c->fault_write = 0;
        return 0;
    }
    return 1;
}
static int MemWrite(A64 *c, uint64_t addr, const void *buf, size_t len) {
    if (c->write == NULL || !c->write(c->memctx, addr, (void *)buf, len)) {
        c->halt = A64_HALT_MEMFAULT;
        c->fault_addr = addr;
        c->fault_size = len;
        c->fault_write = 1;
        return 0;
    }
    return 1;
}

/* Little-endian guest access helpers (host byte order is irrelevant). */
static int LoadLE(A64 *c, uint64_t addr, uint64_t *out, int nbytes) {
    uint8_t b[8];
    if (!MemRead(c, addr, b, (size_t)nbytes)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < nbytes; ++i) v |= (uint64_t)b[i] << (8 * i);
    *out = v;
    return 1;
}
static int StoreLE(A64 *c, uint64_t addr, uint64_t v, int nbytes) {
    uint8_t b[8];
    for (int i = 0; i < nbytes; ++i) b[i] = (uint8_t)(v >> (8 * i));
    { /* TEMP debug: EDEN_WATCH=lo:hi logs stores into [lo,hi). */
        static int winit = 0; static uint64_t wlo = 0, whi = 0; static long wcount = 0;
        if (!winit) {
            const char *e = getenv("EDEN_WATCH");
            if (e) { const char *cp = strchr(e, ':'); wlo = strtoull(e, NULL, 0); if (cp) whi = strtoull(cp + 1, NULL, 0); }
            winit = 1;
        }
        if (whi > wlo && addr < whi && addr + (uint64_t)nbytes > wlo && wcount < 200) {
            fprintf(stderr, "WATCH pc=0x%llx st=0x%llx v=0x%llx n=%d x30=0x%llx x29=0x%llx\n",
                    (unsigned long long)(c->pc - 4), (unsigned long long)addr,
                    (unsigned long long)v, nbytes,
                    (unsigned long long)c->x[30], (unsigned long long)c->x[29]);
            ++wcount;
        }
    }
    return MemWrite(c, addr, b, (size_t)nbytes);
}

/* --------------------------------------------------------- error markers */

static void Undef(A64 *c, uint64_t pc) { c->halt = A64_HALT_UNDEFINED; c->undefined_pc = pc; }
static void Unimpl(A64 *c, uint64_t pc) { c->halt = A64_HALT_UNIMPLEMENTED; c->undefined_pc = pc; }

/* Advanced SIMD / FP handlers defined further down. */
static void DoSimd(A64 *c, uint32_t insn, uint64_t pc);
static void DoSimdPair(A64 *c, uint32_t insn, uint64_t pc);
static void DoSimdMem(A64 *c, unsigned size, unsigned L, uint64_t addr, unsigned Rt);

/* ===================== data processing -- immediate ===================== */

static void DoDPI(A64 *c, uint32_t insn, uint64_t pc) {
    unsigned op1 = Field(insn, 25, 23);
    if (op1 == 0 || op1 == 1) { /* PC-relative */
        unsigned Rd = Field(insn, 4, 0);
        unsigned op = Field(insn, 31, 31); /* 0 = ADR, 1 = ADRP */
        uint64_t imm21 = ((uint64_t)Field(insn, 23, 5) << 2) | Field(insn, 30, 29);
        if (op == 0) { /* ADR */
            WriteX(c, Rd, pc + SignExtend(imm21, 21));
        } else {      /* ADRP */
            WriteX(c, Rd, (pc & ~0xFFFULL) + (SignExtend(imm21, 21) << 12));
        }
        return;
    }
    if (op1 == 2) { /* add/sub immediate */
        unsigned sf = Field(insn, 31, 31), op = Field(insn, 30, 30), S = Field(insn, 29, 29);
        unsigned sh = Field(insn, 22, 22);
        uint64_t imm12 = Field(insn, 21, 10);
        unsigned Rn = Field(insn, 9, 5), Rd = Field(insn, 4, 0);
        int regsize = sf ? 64 : 32;
        uint64_t imm = sh ? (imm12 << 12) : imm12;
        uint64_t a = regsize == 64 ? ReadXSp(c, Rn) : (uint32_t)ReadXSp(c, Rn);
        uint32_t saved = c->nzcv;
        uint64_t result = op ? AddWithCarry(c, a, ~imm, 1, regsize)
                             : AddWithCarry(c, a, imm, 0, regsize);
        if (!S) c->nzcv = saved;
        if (S && Rd == 31) { /* flags only */ }
        else if (regsize == 64) WriteXSp(c, Rd, result);
        else WriteWSp(c, Rd, (uint32_t)result);
        return;
    }
    if (op1 == 4) { /* logical immediate */
        unsigned sf = Field(insn, 31, 31), opc = Field(insn, 30, 29);
        unsigned N = Field(insn, 22, 22), immr = Field(insn, 21, 16), imms = Field(insn, 15, 10);
        unsigned Rn = Field(insn, 9, 5), Rd = Field(insn, 4, 0);
        int regsize = sf ? 64 : 32;
        uint64_t wmask;
        if (!DecodeLogImm(N, imms, immr, regsize, &wmask)) { Undef(c, pc); return; }
        uint64_t a = ReadX(c, Rn);
        uint64_t result;
        switch (opc) {
            case 0: result = a & wmask; break;   /* AND */
            case 1: result = a | wmask; break;   /* ORR */
            case 2: result = a ^ wmask; break;   /* EOR */
            default: result = a & wmask; break;  /* ANDS */
        }
        if (regsize == 32) result &= 0xFFFFFFFFu;
        if (opc == 3) SetLogicFlags(c, result, regsize);
        WriteX(c, Rd, result);
        return;
    }
    if (op1 == 5) { /* move wide */
        unsigned sf = Field(insn, 31, 31), opc = Field(insn, 30, 29);
        unsigned hw = Field(insn, 22, 21);
        uint64_t imm16 = Field(insn, 20, 5);
        unsigned Rd = Field(insn, 4, 0);
        int regsize = sf ? 64 : 32;
        if (regsize == 32 && hw >= 2) { Undef(c, pc); return; }
        uint64_t imm = imm16 << (hw * 16);
        uint64_t result;
        if (opc == 0) result = ~imm;                     /* MOVN */
        else if (opc == 2) result = imm;                 /* MOVZ */
        else if (opc == 3) {                             /* MOVK */
            uint64_t mask = (uint64_t)0xFFFF << (hw * 16);
            result = (ReadX(c, Rd) & ~mask) | imm;
        } else { Undef(c, pc); return; }
        if (regsize == 32) result &= 0xFFFFFFFFu;
        WriteX(c, Rd, result);
        return;
    }
    if (op1 == 6) { /* bitfield */
        unsigned sf = Field(insn, 31, 31), opc = Field(insn, 30, 29), N = Field(insn, 22, 22);
        unsigned immr = Field(insn, 21, 16), imms = Field(insn, 15, 10);
        unsigned Rn = Field(insn, 9, 5), Rd = Field(insn, 4, 0);
        int regsize = sf ? 64 : 32;
        uint64_t mask = (regsize == 32) ? 0xFFFFFFFFull : ~0ull;
        if (N != sf) { Undef(c, pc); return; }
        uint64_t src = ReadX(c, Rn) & mask;
        uint64_t dst = ReadX(c, Rd) & mask;
        uint64_t result;
        if (opc == 2) { /* UBFM */
            if (imms >= immr) result = (src >> immr) & Ones((int)(imms - immr + 1));
            else result = ((src & Ones((int)(imms + 1))) << (regsize - immr)) & mask;
        } else if (opc == 0) { /* SBFM */
            if (imms >= immr) {
                int w = (int)(imms - immr + 1);
                result = SignExtend((src >> immr) & Ones(w), w);
            } else {
                int w = (int)(imms + 1);
                result = SignExtend(src & Ones(w), w) << (regsize - immr);
            }
            result &= mask;
        } else if (opc == 1) { /* BFM */
            if (imms >= immr) {
                int w = (int)(imms - immr + 1);
                uint64_t field = (src >> immr) & Ones(w);
                result = (dst & ~Ones(w)) | field;
            } else {
                int w = (int)(imms + 1);
                uint64_t shift = (uint64_t)(regsize - immr);
                uint64_t m = Ones(w) << shift;
                uint64_t field = (src & Ones(w)) << shift;
                result = (dst & ~m) | field;
            }
        } else { Undef(c, pc); return; }
        if (regsize == 32) result &= 0xFFFFFFFFu;
        WriteX(c, Rd, result);
        return;
    }
    if (op1 == 7) { /* EXTR */
        unsigned sf = Field(insn, 31, 31), op21 = Field(insn, 30, 29), N = Field(insn, 22, 22);
        unsigned o0 = Field(insn, 21, 21), Rm = Field(insn, 20, 16), imms = Field(insn, 15, 10);
        unsigned Rn = Field(insn, 9, 5), Rd = Field(insn, 4, 0);
        int regsize = sf ? 64 : 32;
        if (op21 != 0 || N != sf || o0 != 0) { Undef(c, pc); return; }
        unsigned lsb = imms & (unsigned)(regsize - 1);
        uint64_t lo = ReadX(c, Rm), hi = ReadX(c, Rn);
        uint64_t result;
        if (regsize == 32) {
            lo &= 0xFFFFFFFFu; hi &= 0xFFFFFFFFu;
            result = lsb ? (((lo >> lsb) | (hi << (32 - lsb))) & 0xFFFFFFFFu) : lo;
        } else {
            result = lsb ? ((lo >> lsb) | (hi << (64 - lsb))) : lo;
        }
        WriteX(c, Rd, result);
        return;
    }
    Undef(c, pc);
}

/* ====================== data processing -- register ==================== */

/* Add/Sub destination semantics: when the instruction sets flags (S=1) and
 * Rd is 31, the result is DISCARDED (that is what makes CMP/CMN/TST work) --
 * it must never touch sp. When S=0, Rd 31 is the stack pointer. */
static void AddSubWriteRd(A64 *c, unsigned S, unsigned Rd, uint64_t result,
                          int regsize) {
    if (S && Rd == 31) return;
    if (regsize == 32) WriteWSp(c, Rd, (uint32_t)result);
    else                WriteXSp(c, Rd, result);
}

static void DoAddSubShifted(A64 *c, uint32_t insn, uint64_t pc) {
    unsigned sf = Field(insn, 31, 31), op = Field(insn, 30, 30), S = Field(insn, 29, 29);
    unsigned shift = Field(insn, 23, 22), Rm = Field(insn, 20, 16), imm6 = Field(insn, 15, 10);
    unsigned Rn = Field(insn, 9, 5), Rd = Field(insn, 4, 0);
    int regsize = sf ? 64 : 32;
    if (!sf && (imm6 >> 5)) { Undef(c, pc); return; }
    /* Shifted-register form: register 31 is XZR, never SP (SP is only
     * encodable in the extended/immediate forms). */
    uint64_t a = ReadX(c, Rn);
    uint64_t b = ShiftVal(ReadX(c, Rm), shift, imm6, regsize);
    uint32_t saved = c->nzcv;
    uint64_t result = op ? AddWithCarry(c, a, ~b, 1, regsize) : AddWithCarry(c, a, b, 0, regsize);
    if (!S) c->nzcv = saved;
    if (regsize == 32) result &= 0xFFFFFFFFu;
    WriteX(c, Rd, result);
}

static void DoLogicalShifted(A64 *c, uint32_t insn, uint64_t pc) {
    unsigned sf = Field(insn, 31, 31), opc = Field(insn, 30, 29), shift = Field(insn, 23, 22);
    unsigned N = Field(insn, 21, 21), Rm = Field(insn, 20, 16), imm6 = Field(insn, 15, 10);
    unsigned Rn = Field(insn, 9, 5), Rd = Field(insn, 4, 0);
    int regsize = sf ? 64 : 32;
    if ((!sf && (imm6 >> 5))) { Undef(c, pc); return; }
    uint64_t a = ReadX(c, Rn);
    uint64_t b = ShiftVal(ReadX(c, Rm), shift, imm6, regsize);
    if (N) b = ~b;
    uint64_t result;
    switch (opc) {
        case 0: result = a & b; break;   /* AND / BIC */
        case 1: result = a | b; break;   /* ORR / ORN */
        case 2: result = a ^ b; break;   /* EOR / EON */
        default: result = a & b; break;  /* ANDS / BICS */
    }
    if (regsize == 32) result &= 0xFFFFFFFFu;
    if (opc == 3) SetLogicFlags(c, result, regsize);
    WriteX(c, Rd, result);
}

static void DoAddSubExtended(A64 *c, uint32_t insn, uint64_t pc) {
    unsigned sf = Field(insn, 31, 31), op = Field(insn, 30, 30), S = Field(insn, 29, 29);
    unsigned opt = Field(insn, 23, 22), Rm = Field(insn, 20, 16), option = Field(insn, 15, 13);
    unsigned imm3 = Field(insn, 12, 10), Rn = Field(insn, 9, 5), Rd = Field(insn, 4, 0);
    int regsize = sf ? 64 : 32;
    if (opt != 0 || imm3 > 4) { Undef(c, pc); return; }
    uint64_t a = ReadXSp(c, Rn);
    if (regsize == 32) a &= 0xFFFFFFFFu;
    uint64_t b = ExtendVal(ReadX(c, Rm), option) << imm3;
    if (regsize == 32) b &= 0xFFFFFFFFu;
    uint32_t saved = c->nzcv;
    uint64_t result = op ? AddWithCarry(c, a, ~b, 1, regsize) : AddWithCarry(c, a, b, 0, regsize);
    if (!S) c->nzcv = saved;
    if (regsize == 32) result &= 0xFFFFFFFFu;
    AddSubWriteRd(c, S, Rd, result, regsize);
}

static void DoAddSubCarry(A64 *c, uint32_t insn, uint64_t pc) {
    (void)pc;
    unsigned sf = Field(insn, 31, 31), op = Field(insn, 30, 30), S = Field(insn, 29, 29);
    unsigned Rm = Field(insn, 20, 16), Rn = Field(insn, 9, 5), Rd = Field(insn, 4, 0);
    int regsize = sf ? 64 : 32;
    uint32_t cin = (c->nzcv & A64_NZCV_C) ? 1u : 0u;
    uint64_t a = ReadX(c, Rn), b = ReadX(c, Rm);
    uint32_t saved = c->nzcv;
    uint64_t result = AddWithCarry(c, a, op ? ~b : b, cin, regsize);
    if (!S) c->nzcv = saved;
    if (regsize == 32) result &= 0xFFFFFFFFu;
    WriteX(c, Rd, result);
}

static void DoCondSelect(A64 *c, uint32_t insn, uint64_t pc) {
    (void)pc;
    unsigned op = Field(insn, 30, 30), Rm = Field(insn, 20, 16), cond = Field(insn, 15, 12);
    unsigned o2 = Field(insn, 10, 10), Rn = Field(insn, 9, 5), Rd = Field(insn, 4, 0);
    int regsize = Field(insn, 31, 31) ? 64 : 32;
    uint64_t result;
    if (CondCheck(c->nzcv, cond)) {
        result = ReadX(c, Rn);
    } else {
        uint64_t b = ReadX(c, Rm);
        if (op) result = o2 ? (uint64_t)(0 - b) : ~b;   /* CSNEG : CSINV */
        else    result = o2 ? (b + 1)        : b;        /* CSINC : CSEL  */
    }
    if (regsize == 32) result &= 0xFFFFFFFFu;
    WriteX(c, Rd, result);
}

static void DoCondCompare(A64 *c, uint32_t insn, uint64_t pc) {
    unsigned sf = Field(insn, 31, 31), op = Field(insn, 30, 30);
    unsigned imm_form = Field(insn, 11, 11);            /* 1 = CCMP/CCMN #imm5 */
    unsigned operand = Field(insn, 20, 16);             /* Rm or imm5 */
    unsigned cond = Field(insn, 15, 12), Rn = Field(insn, 9, 5);
    unsigned nzcv_imm = Field(insn, 3, 0);
    int regsize = sf ? 64 : 32;
    (void)pc;
    if (CondCheck(c->nzcv, cond)) {
        uint64_t a = ReadX(c, Rn);
        uint64_t b = imm_form ? (uint64_t)operand : ReadX(c, operand);
        if (op == 0) AddWithCarry(c, a, b, 0, regsize);
        else AddWithCarry(c, a, ~b, 1, regsize);
    } else {
        c->nzcv = nzcv_imm << 28;
    }
}

static void DoTwoSource(A64 *c, uint32_t insn, uint64_t pc) {
    unsigned sf = Field(insn, 31, 31), opcode = Field(insn, 15, 10), Rm = Field(insn, 20, 16);
    unsigned Rn = Field(insn, 9, 5), Rd = Field(insn, 4, 0);
    int regsize = sf ? 64 : 32;
    uint64_t a = ReadX(c, Rn), b = ReadX(c, Rm);
    if (regsize == 32) { a &= 0xFFFFFFFFu; b &= 0xFFFFFFFFu; }
    uint64_t result = 0;
    if (opcode == 2) { /* UDIV */
        if (b == 0) result = 0;
        else result = regsize == 64 ? (a / b) : ((uint32_t)a / (uint32_t)b);
    } else if (opcode == 3) { /* SDIV */
        if (regsize == 64) {
            if (b == 0) result = 0;
            else {
                int64_t sa = (int64_t)a, sb = (int64_t)b;
                if (sa == (-9223372036854775807LL - 1) && sb == -1) result = (uint64_t)sa;
                else result = (uint64_t)(sa / sb);
            }
        } else {
            if (b == 0) result = 0;
            else {
                int32_t sa = (int32_t)a, sb = (int32_t)b;
                if (sa == (-2147483647 - 1) && sb == -1) result = (uint32_t)sa;
                else result = (uint32_t)(sa / sb);
            }
        }
    } else if (opcode >= 8 && opcode <= 11) {
        unsigned amount = (unsigned)(b & (regsize - 1));
        result = ShiftVal(a, opcode - 8, amount, regsize);
    } else { Undef(c, pc); return; }
    if (regsize == 32) result &= 0xFFFFFFFFu;
    WriteX(c, Rd, result);
}

static void DoOneSource(A64 *c, uint32_t insn, uint64_t pc) {
    unsigned sf = Field(insn, 31, 31), opcode2 = Field(insn, 20, 16), opcode = Field(insn, 15, 10);
    unsigned Rn = Field(insn, 9, 5), Rd = Field(insn, 4, 0);
    int regsize = sf ? 64 : 32;
    if (opcode2 != 0) { Undef(c, pc); return; }
    uint64_t a = ReadX(c, Rn);
    uint64_t result;
    switch (opcode) {
        case 0: result = BitReverse(a, regsize); break; /* RBIT */
        case 1: { /* REV16 */
            if (regsize == 64) {
                result =  RevBytes(a & 0xFFFF, 2) |
                         (RevBytes((a >> 16) & 0xFFFF, 2) << 16) |
                         (RevBytes((a >> 32) & 0xFFFF, 2) << 32) |
                         (RevBytes((a >> 48) & 0xFFFF, 2) << 48);
            } else {
                result = RevBytes(a & 0xFFFF, 2) | (RevBytes((a >> 16) & 0xFFFF, 2) << 16);
            }
        } break;
        case 2: { /* REV32 (64-bit only) */
            if (regsize != 64) { Undef(c, pc); return; }
            result = (RevBytes((a >> 32) & 0xFFFFFFFF, 4) << 32) | RevBytes(a & 0xFFFFFFFF, 4);
        } break;
        case 3: result = RevBytes(a, regsize / 8); break; /* REV */
        case 4: result = regsize == 64 ? Clz64(a) : Clz32((uint32_t)a); break; /* CLZ */
        case 5: result = ClsCount(a, regsize); break; /* CLS */
        default: Undef(c, pc); return;
    }
    if (regsize == 32) result &= 0xFFFFFFFFu;
    WriteX(c, Rd, result);
}

static void DoThreeSource(A64 *c, uint32_t insn, uint64_t pc) {
    unsigned sf = Field(insn, 31, 31), op54 = Field(insn, 30, 29), opcode = Field(insn, 23, 21);
    unsigned Rm = Field(insn, 20, 16), o0 = Field(insn, 15, 15), Ra = Field(insn, 14, 10);
    unsigned Rn = Field(insn, 9, 5), Rd = Field(insn, 4, 0);
    int regsize = sf ? 64 : 32;
    if (op54 != 0) { Undef(c, pc); return; }
    if (opcode == 0) { /* MADD / MSUB */
        uint64_t a = ReadX(c, Rn), b = ReadX(c, Rm), acc = ReadX(c, Ra);
        if (regsize == 32) { a &= 0xFFFFFFFFu; b &= 0xFFFFFFFFu; acc &= 0xFFFFFFFFu; }
        uint64_t prod = a * b;
        uint64_t result = o0 ? acc - prod : acc + prod;
        if (regsize == 32) result &= 0xFFFFFFFFu;
        WriteX(c, Rd, result);
    } else if (opcode == 1) { /* SMADDL / SMSUBL */
        if (regsize != 64) { Undef(c, pc); return; }
        int64_t a = (int32_t)ReadW(c, Rn), b = (int32_t)ReadW(c, Rm);
        int64_t acc = (int64_t)ReadX(c, Ra);
        int64_t prod = a * b;
        WriteX(c, Rd, (uint64_t)(o0 ? acc - prod : acc + prod));
    } else if (opcode == 2) { /* SMULH */
        if (regsize != 64) { Undef(c, pc); return; }
        uint64_t a = ReadX(c, Rn), b = ReadX(c, Rm);
        uint64_t hi = MulHi64(a, b);
        if ((int64_t)a < 0) hi -= b;
        if ((int64_t)b < 0) hi -= a;
        WriteX(c, Rd, hi);
    } else if (opcode == 5) { /* UMADDL / UMSUBL */
        if (regsize != 64) { Undef(c, pc); return; }
        uint64_t a = ReadW(c, Rn), b = ReadW(c, Rm), acc = ReadX(c, Ra);
        uint64_t prod = a * b;
        WriteX(c, Rd, o0 ? acc - prod : acc + prod);
    } else if (opcode == 6) { /* UMULH */
        if (regsize != 64) { Undef(c, pc); return; }
        WriteX(c, Rd, MulHi64(ReadX(c, Rn), ReadX(c, Rm)));
    } else {
        Undef(c, pc);
    }
}

static void DoDPR(A64 *c, uint32_t insn, uint64_t pc) {
    uint32_t b28_24 = Field(insn, 28, 24);
    unsigned bit21 = Field(insn, 21, 21);
    if (b28_24 == 0x0B) { if (!bit21) DoAddSubShifted(c, insn, pc); else DoAddSubExtended(c, insn, pc); return; }
    if (b28_24 == 0x0A) { DoLogicalShifted(c, insn, pc); return; }
    if (b28_24 == 0x1B) { DoThreeSource(c, insn, pc); return; }
    if (b28_24 == 0x1A) {
        unsigned op3 = Field(insn, 23, 21);
        if (op3 == 0) { DoAddSubCarry(c, insn, pc); return; }
        if (op3 == 2) { DoCondCompare(c, insn, pc); return; }
        if (op3 == 4) { DoCondSelect(c, insn, pc); return; }
        if (op3 == 6) {
            if (Field(insn, 30, 30)) DoOneSource(c, insn, pc);
            else DoTwoSource(c, insn, pc);
            return;
        }
    }
    Undef(c, pc);
}

/* ============================ loads / stores =========================== */

static void DoMemAccess(A64 *c, unsigned size, unsigned opc, uint64_t addr, unsigned Rt) {
    int nbytes = 1 << size;
    if (opc == 0) { /* store */
        uint64_t v = ReadX(c, Rt);
        if (!StoreLE(c, addr, v, nbytes)) return;
        return;
    }
    int bits = 8 * nbytes;
    uint64_t raw = 0;
    if (!LoadLE(c, addr, &raw, nbytes)) return;
    if (opc == 1) { /* load, zero-extend */
        if (size == 3) WriteX(c, Rt, raw);
        else WriteW(c, Rt, (uint32_t)raw);
    } else if (opc == 2) { /* load, sign-extend to X */
        if (size == 3) { Unimpl(c, 0); return; } /* would be PRFM */
        WriteX(c, Rt, SignExtend(raw, bits));
    } else { /* opc == 3: sign-extend to W */
        if (size >= 2) { Undef(c, 0); return; }
        WriteW(c, Rt, (uint32_t)SignExtend(raw, bits));
    }
}

static void DoRegUnsignedImm(A64 *c, uint32_t insn, uint64_t pc) {
    unsigned size = Field(insn, 31, 30), opc = Field(insn, 23, 22);
    unsigned Rn = Field(insn, 9, 5), Rt = Field(insn, 4, 0);
    uint64_t imm12 = Field(insn, 21, 10);
    if (size == 3 && opc == 2) { Unimpl(c, pc); return; } /* PRFM */
    uint64_t addr = ReadXSp(c, Rn) + (imm12 << size);
    DoMemAccess(c, size, opc, addr, Rt);
}

static void DoRegForm(A64 *c, uint32_t insn, uint64_t pc) {
    unsigned size = Field(insn, 31, 30), opc = Field(insn, 23, 22);
    unsigned Rn = Field(insn, 9, 5), Rt = Field(insn, 4, 0);
    unsigned bit21 = Field(insn, 21, 21);
    int64_t imm9 = SignExtend(Field(insn, 20, 12), 9);
    unsigned idx = Field(insn, 11, 10);
    uint64_t base = ReadXSp(c, Rn);
    if (bit21 && idx != 2) { Unimpl(c, pc); return; } /* atomic memory ops */
    if (idx == 2) { /* register offset */
        unsigned option = Field(insn, 15, 13), S = Field(insn, 12, 12), Rm = Field(insn, 20, 16);
        unsigned sh = S ? size : 0;
        uint64_t off = ExtendVal(ReadX(c, Rm), option) << sh;
        DoMemAccess(c, size, opc, base + off, Rt);
        return;
    }
    uint64_t addr;
    int writeback = 0;
    if (idx == 0) { addr = base + (uint64_t)imm9; }
    else if (idx == 1) { addr = base; writeback = 1; }               /* post-index */
    else { addr = base + (uint64_t)imm9; writeback = 1; }            /* pre-index  */
    DoMemAccess(c, size, opc, addr, Rt);
    if (c->halt != A64_OK) return;
    if (writeback) {
        uint64_t nb = base + (uint64_t)imm9;
        if (size == 3 || opc != 1) { /* 64-bit transfer target */ }
        WriteXSp(c, Rn, nb);
    }
}

static void DoLiteral(A64 *c, uint32_t insn, uint64_t pc) {
    unsigned opc = Field(insn, 31, 30), V = Field(insn, 26, 26);
    uint64_t imm19 = Field(insn, 23, 5);
    unsigned Rt = Field(insn, 4, 0);
    uint64_t addr = pc + (SignExtend(imm19, 19) << 2);
    if (V) {
        /* SIMD&FP literal: opc 0=S(32), 1=D(64), 2=Q(128). */
        unsigned size = (opc == 2) ? 0 : (opc == 1 ? 3 : 2);
        DoSimdMem(c, size, 1, addr, Rt);
        return;
    }
    if (opc == 3) { return; } /* PRFM, no-op */
    int nbytes = (opc == 1 || opc == 2) ? 8 : 4;
    uint64_t raw = 0;
    if (!LoadLE(c, addr, &raw, nbytes)) return;
    if (opc == 0) WriteW(c, Rt, (uint32_t)raw);
    else if (opc == 1) WriteX(c, Rt, raw);
    else WriteX(c, Rt, SignExtend(raw, 32)); /* LDRSW */
}

static void DoPair(A64 *c, uint32_t insn, uint64_t pc) {
    unsigned opc = Field(insn, 31, 30), V = Field(insn, 26, 26);
    unsigned index = Field(insn, 24, 23), L = Field(insn, 22, 22);
    int64_t imm7 = SignExtend(Field(insn, 21, 15), 7);
    unsigned Rt2 = Field(insn, 14, 10), Rn = Field(insn, 9, 5), Rt = Field(insn, 4, 0);
    if (V) { DoSimdPair(c, insn, pc); return; }
    int scale = (opc == 2) ? 3 : 2;
    int64_t offset = imm7 * (1 << scale);
    uint64_t base = ReadXSp(c, Rn);
    uint64_t addr = (index == 1) ? base : (uint64_t)((int64_t)base + offset);
    int nbytes = 1 << scale;
    if (L) {
        uint64_t v1, v2;
        if (!LoadLE(c, addr, &v1, nbytes)) return;
        if (!LoadLE(c, addr + nbytes, &v2, nbytes)) return;
        if (opc == 2) { WriteX(c, Rt, v1); WriteX(c, Rt2, v2); }
        else if (opc == 1) { WriteX(c, Rt, SignExtend(v1, 32)); WriteX(c, Rt2, SignExtend(v2, 32)); }
        else { WriteW(c, Rt, (uint32_t)v1); WriteW(c, Rt2, (uint32_t)v2); }
    } else {
        uint64_t v1 = ReadX(c, Rt), v2 = ReadX(c, Rt2);
        if (!StoreLE(c, addr, v1, nbytes)) return;
        if (!StoreLE(c, addr + nbytes, v2, nbytes)) return;
    }
    if (index == 1 || index == 3) { /* post / pre: writeback */
        WriteXSp(c, Rn, (uint64_t)((int64_t)base + offset));
    }
}

static void DoExclusive(A64 *c, uint32_t insn, uint64_t pc) {
    (void)pc;
    unsigned size = Field(insn, 31, 30), L = Field(insn, 22, 22);
    unsigned Rs = Field(insn, 20, 16), Rt2 = Field(insn, 14, 10);
    unsigned Rn = Field(insn, 9, 5), Rt = Field(insn, 4, 0);
    (void)Rt2;
    uint64_t addr = ReadXSp(c, Rn);
    /* Single-threaded simplification: exclusives always succeed. */
    if (L) {
        DoMemAccess(c, size, 1, addr, Rt);
    } else {
        DoMemAccess(c, size, 0, addr, Rt);
        if (c->halt == A64_OK && Rs != 31) WriteW(c, Rs, 0);
    }
}

/* ===================== Advanced SIMD and floating point ================ */

/* Element size in bits for a 2-bit "size" field: 0=8b, 1=16b, 2=32b, 3=64b. */
static unsigned VEBits(unsigned size) { return 8u << size; }

static void VSetElem(A64 *c, unsigned n, unsigned size, unsigned idx, uint64_t val) {
    unsigned bits = VEBits(size);
    unsigned off = idx * bits;
    uint64_t mask = (bits >= 64) ? ~0ull : ((1ull << bits) - 1);
    c->v[n][off >> 6] = (c->v[n][off >> 6] & ~(mask << (off & 63))) |
                        ((val & mask) << (off & 63));
}

static uint64_t VGetElem(A64 *c, unsigned n, unsigned size, unsigned idx) {
    unsigned bits = VEBits(size);
    unsigned off = idx * bits;
    uint64_t mask = (bits >= 64) ? ~0ull : ((1ull << bits) - 1);
    return (c->v[n][off >> 6] >> (off & 63)) & mask;
}

/* Number of active elements for an arrangement of `size` lanes at width Q. */
static unsigned VLanes(unsigned size, unsigned Q) {
    return (Q ? 128u : 64u) / VEBits(size);
}

/* Write a scalar result into Vn, zeroing the rest of the register. */
static void VWriteScalar(A64 *c, unsigned n, unsigned bits, uint64_t val) {
    uint64_t mask = (bits >= 64) ? ~0ull : ((1ull << bits) - 1);
    c->v[n][0] = val & mask;
    c->v[n][1] = 0;
}

/* ---- SIMD&FP load/store, single register ---- */

static void DoSimdMem(A64 *c, unsigned size, unsigned L, uint64_t addr, unsigned Rt) {
    /* size (SIMD load/store): 0=128-bit(Q), 1=8-bit(B), 2=32-bit(S), 3=64-bit(D). */
    static const int nbytes_tab[4] = { 16, 1, 4, 8 };
    int nbytes = nbytes_tab[size & 3];
    if (nbytes > 8) {
        uint64_t lo = c->v[Rt][0], hi = c->v[Rt][1];
        if (L) {
            if (!LoadLE(c, addr, &lo, 8)) return;
            if (!LoadLE(c, addr + 8, &hi, 8)) return;
            c->v[Rt][0] = lo; c->v[Rt][1] = hi;
        } else {
            if (!StoreLE(c, addr, lo, 8)) return;
            if (!StoreLE(c, addr + 8, hi, 8)) return;
        }
    } else {
        uint64_t v = c->v[Rt][0];
        if (L) {
            if (!LoadLE(c, addr, &v, nbytes)) return;
            VWriteScalar(c, Rt, 8 * nbytes, v);
        } else {
            if (!StoreLE(c, addr, v, nbytes)) return;
        }
    }
}

static void DoSimdUnsignedImm(A64 *c, uint32_t insn, uint64_t pc) {
    unsigned size = Field(insn, 31, 30);
    unsigned opc  = Field(insn, 23, 22);
    unsigned Rn = Field(insn, 9, 5), Rt = Field(insn, 4, 0);
    uint64_t imm12 = Field(insn, 21, 10);
    static const unsigned scale[4] = { 4, 0, 2, 3 }; /* bytes = 1<<scale */
    unsigned L = opc & 1; /* bit0: 0 store, 1 load (privilege variants ignored) */
    uint64_t addr = ReadXSp(c, Rn) + (imm12 << scale[size & 3]);
    DoSimdMem(c, size, L, addr, Rt);
    (void)pc;
}

static void DoSimdRegForm(A64 *c, uint32_t insn, uint64_t pc) {
    unsigned size = Field(insn, 31, 30);
    unsigned opc  = Field(insn, 23, 22);
    unsigned Rn = Field(insn, 9, 5), Rt = Field(insn, 4, 0);
    int64_t imm9 = SignExtend(Field(insn, 20, 12), 9);
    unsigned idx = Field(insn, 11, 10);
    unsigned L = opc & 1;
    uint64_t base = ReadXSp(c, Rn);
    if (idx == 2) { Unimpl(c, pc); return; } /* SIMD register-offset: unused here */
    uint64_t addr;
    int wb = 0;
    if (idx == 0) { addr = base + (uint64_t)imm9; }             /* unscaled */
    else if (idx == 1) { addr = base; wb = 1; }                 /* post-index */
    else { addr = base + (uint64_t)imm9; wb = 1; }              /* pre-index */
    DoSimdMem(c, size, L, addr, Rt);
    if (c->halt != A64_OK) return;
    if (wb) WriteXSp(c, Rn, base + (uint64_t)imm9);
}

/* ---- SIMD&FP load/store, register pair ---- */

static void DoSimdPair(A64 *c, uint32_t insn, uint64_t pc) {
    unsigned opc = Field(insn, 31, 30);
    unsigned index = Field(insn, 24, 23), L = Field(insn, 22, 22);
    int64_t imm7 = SignExtend(Field(insn, 21, 15), 7);
    unsigned Rt2 = Field(insn, 14, 10), Rn = Field(insn, 9, 5), Rt = Field(insn, 4, 0);
    int nbytes;
    if (opc == 0)      nbytes = 4;   /* S */
    else if (opc == 1) nbytes = 8;   /* D */
    else if (opc == 2) nbytes = 16;  /* Q */
    else { Undef(c, pc); return; }
    int64_t offset = imm7 * nbytes;
    uint64_t base = ReadXSp(c, Rn);
    uint64_t addr = (index == 1) ? base : (uint64_t)((int64_t)base + offset);

    unsigned regs[2] = { Rt, Rt2 };
    unsigned k;
    for (k = 0; k < 2; ++k) {
        uint64_t a = addr + (uint64_t)k * (uint64_t)nbytes;
        unsigned r = regs[k];
        if (L) {
            uint64_t lo = c->v[r][0], hi = 0;
            int first = (nbytes > 8) ? 8 : nbytes;
            if (!LoadLE(c, a, &lo, first)) return;
            if (nbytes > 8 && !LoadLE(c, a + 8, &hi, 8)) return;
            c->v[r][0] = lo; c->v[r][1] = (nbytes > 8) ? hi : 0;
        } else {
            int first = (nbytes > 8) ? 8 : nbytes;
            if (!StoreLE(c, a, c->v[r][0], first)) return;
            if (nbytes > 8 && !StoreLE(c, a + 8, c->v[r][1], 8)) return;
        }
    }
    if (index == 1 || index == 3) WriteXSp(c, Rn, (uint64_t)((int64_t)base + offset));
}

/* ---- scalar FP helpers ---- */

static float FBitsToF32(uint64_t u) { float f; uint32_t v = (uint32_t)u; memcpy(&f, &v, 4); return f; }
static uint64_t F32ToFBits(float f) { uint32_t v; memcpy(&v, &f, 4); return (uint64_t)v; }
static double FBitsToF64(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static uint64_t F64ToFBits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static float FMaxNum(float a, float b) { return (a > b) ? a : b; }
static float FMinNum(float a, float b) { return (a < b) ? a : b; }
static double DMaxNum(double a, double b) { return (a > b) ? a : b; }
static double DMinNum(double a, double b) { return (a < b) ? a : b; }

/* ---- Advanced SIMD data processing ---- */

/* AdvSIMDExpandImm (ARM ARM): expand the 8-bit modified immediate to 64 bits. */
static uint64_t AdvSimdExpandImmBits(unsigned op, unsigned cmode, unsigned imm8) {
    uint64_t v = 0;
    unsigned i;
    switch (cmode >> 1) {
    case 0: v = (uint64_t)(uint32_t)imm8;            v |= v << 32; break;
    case 1: v = (uint64_t)(uint32_t)(imm8 << 8);     v |= v << 32; break;
    case 2: v = (uint64_t)(uint32_t)(imm8 << 16);    v |= v << 32; break;
    case 3: v = (uint64_t)(uint32_t)(imm8 << 24);    v |= v << 32; break;
    case 4: v = (uint64_t)(uint16_t)(imm8 << 8);     v |= v << 16; v |= v << 32; break;
    case 5: v = (uint64_t)(uint16_t)imm8;            v |= v << 16; v |= v << 32; break;
    case 6:
        v = (uint64_t)(uint32_t)(((cmode & 1u) == 0) ? ((imm8 << 8) | 0xffu)
                                                     : ((imm8 << 16) | 0xffffu));
        v |= v << 32;
        break;
    case 7:
        if ((cmode & 1u) == 0 && op == 0) {          /* 8-bit broadcast */
            for (i = 0; i < 8; ++i) v |= (uint64_t)imm8 << (8 * i);
        } else if ((cmode & 1u) == 0 && op == 1) {    /* 64-bit, bit-replicated bytes */
            for (i = 0; i < 8; ++i) {
                unsigned bit = (imm8 >> (7 - i)) & 1u;
                v |= (uint64_t)(bit ? 0xFFu : 0u) << (8 * i);
            }
        } else if ((cmode & 1u) == 1 && op == 0) {    /* F32 */
            unsigned s = (imm8 >> 7) & 1u, e6 = (imm8 >> 6) & 1u;
            uint32_t f = ((uint32_t)s << 31) | ((uint32_t)(e6 ^ 1u) << 30) |
                         (e6 ? (0x1Fu << 25) : 0u) | ((uint32_t)(imm8 & 0x3Fu) << 19);
            v = (uint64_t)f; v |= v << 32;
        } else {                                       /* F64 */
            unsigned s = (imm8 >> 7) & 1u, e6 = (imm8 >> 6) & 1u;
            v = ((uint64_t)s << 63) | ((uint64_t)(e6 ^ 1u) << 62) |
                (e6 ? (0xFFull << 54) : 0ull) | ((uint64_t)(imm8 & 0x3Fu) << 48);
        }
        break;
    }
    return v;
}

static void DoSimd(A64 *c, uint32_t insn, uint64_t pc) {
    unsigned Q = Field(insn, 30, 30);

    /* DUP (general): 0 Q 00111000 imm5 0 0001 1 Rn Rd */
    if ((insn & 0xBFE0FC00u) == 0x0E000C00u) {
        unsigned imm5 = Field(insn, 20, 16);
        unsigned Rn = Field(insn, 9, 5), Rd = Field(insn, 4, 0);
        unsigned size, idx;
        if (imm5 & 1) { size = 0; idx = (imm5 >> 1) & 0xF; }         /* 8-bit */
        else if (imm5 & 2) { size = 1; idx = (imm5 >> 2) & 0x7; }    /* 16-bit */
        else if (imm5 & 4) { size = 2; idx = (imm5 >> 3) & 0x3; }    /* 32-bit */
        else if (imm5 & 8) { size = 3; idx = (imm5 >> 4) & 0x1; }    /* 64-bit */
        else { Undef(c, pc); return; }
        unsigned bits = VEBits(size);
        uint64_t src = bits > 32 ? ReadX(c, Rn) : (uint32_t)ReadX(c, Rn);
        uint64_t val = (bits >= 64) ? src : (src & ((1ull << bits) - 1));
        unsigned lanes = VLanes(size, Q);
        for (unsigned i = 0; i < lanes; ++i) VSetElem(c, Rd, size, i, val);
        if (!Q) c->v[Rd][1] = 0;
        (void)idx;
        return;
    }

    /* FMOV (scalar, general <-> FP). */
    if ((insn & 0xFFFFFC00u) == 0x9E670000u) {   /* FMOV Dd, Xn */
        c->v[Field(insn, 4, 0)][0] = ReadX(c, Field(insn, 9, 5));
        c->v[Field(insn, 4, 0)][1] = 0;
        return;
    }
    if ((insn & 0xFFFFFC00u) == 0x9E660000u) {   /* FMOV Xd, Dn */
        WriteX(c, Field(insn, 4, 0), c->v[Field(insn, 9, 5)][0]);
        return;
    }
    if ((insn & 0xFFFFFC00u) == 0x1E270000u) {   /* FMOV Sd, Wn */
        c->v[Field(insn, 4, 0)][0] = (uint32_t)ReadX(c, Field(insn, 9, 5));
        c->v[Field(insn, 4, 0)][1] = 0;
        return;
    }
    if ((insn & 0xFFFFFC00u) == 0x1E260000u) {   /* FMOV Wd, Sn */
        WriteX(c, Field(insn, 4, 0), (uint32_t)c->v[Field(insn, 9, 5)][0]);
        return;
    }

    /* INS / MOV (to vector element, from general): imm5 selects size/lane. */
    if ((insn & 0xBFE0FC00u) == 0x0E001C00u) {
        unsigned imm5 = Field(insn, 20, 16);
        unsigned Rn = Field(insn, 9, 5), Rd = Field(insn, 4, 0);
        unsigned size, idx;
        if (imm5 & 1) { size = 0; idx = (imm5 >> 1) & 0xF; }
        else if (imm5 & 2) { size = 1; idx = (imm5 >> 2) & 0x7; }
        else if (imm5 & 4) { size = 2; idx = (imm5 >> 3) & 0x3; }
        else if (imm5 & 8) { size = 3; idx = (imm5 >> 4) & 0x1; }
        else { Undef(c, pc); return; }
        VSetElem(c, Rd, size, idx, ReadX(c, Rn));
        return;
    }

    /* FMOV (scalar, immediate): 0 0 0 11110 ftype 1 imm8 10000000 Rd. */
    if ((insn & 0xFF000000u) == 0x1E000000u &&
        (insn & 0x00001FE0u) == 0x00001000u) {
        unsigned ftype = Field(insn, 23, 22);
        unsigned Rd = Field(insn, 4, 0);
        unsigned imm8 = Field(insn, 20, 13);
        unsigned b6 = (imm8 >> 6) & 1u, b7 = (imm8 >> 7) & 1u;
        if (ftype == 0) {              /* single */
            uint64_t imm = (b7 ? 0x8000u : 0u) | (b6 ? 0x3E00u : 0x4000u)
                         | ((uint64_t)(imm8 & 0x3Fu) << 3);
            VWriteScalar(c, Rd, 32, (uint32_t)(imm << 16));
            return;
        } else if (ftype == 1) {       /* double */
            uint64_t imm = (b7 ? 0x8000ull : 0ull) | (b6 ? 0x3FC0ull : 0x4000ull)
                         | (uint64_t)(imm8 & 0x3Fu);
            VWriteScalar(c, Rd, 64, imm << 48);
            return;
        } else if (ftype == 3) {       /* half */
            uint64_t imm = (b7 ? 0x8000u : 0u) | (b6 ? 0x3000u : 0x4000u)
                         | ((uint64_t)(imm8 & 0x3Fu) << 6);
            VWriteScalar(c, Rd, 16, (uint16_t)imm);
            return;
        }
    }

    /* Advanced SIMD three same: 0 Q U 01110 size 1 Rm opcode 1 Rn Rd. */
    if (Field(insn, 31, 31) == 0 && Field(insn, 28, 24) == 0x0E &&
        Field(insn, 21, 21) == 1 && Field(insn, 10, 10) == 1) {
        unsigned Q = Field(insn, 30, 30), U = Field(insn, 29, 29);
        unsigned size = Field(insn, 23, 22);
        unsigned Rm = Field(insn, 20, 16), opcode = Field(insn, 15, 11);
        unsigned Rn = Field(insn, 9, 5), Rd = Field(insn, 4, 0);
        unsigned ebits = VEBits(size);
        uint64_t emask = (ebits >= 64) ? ~0ull : ((1ull << ebits) - 1);

        if (opcode == 0x03u) {                        /* logical (U,size select op) */
            uint64_t a = c->v[Rn][0], b = c->v[Rm][0], d = c->v[Rd][0];
            uint64_t a1 = c->v[Rn][1], b1 = c->v[Rm][1], d1 = c->v[Rd][1];
            uint64_t r0, r1;
            switch ((U << 2) | size) {
                case 0: r0 = a & b;      r1 = a1 & b1;      break; /* AND */
                case 1: r0 = a & ~b;     r1 = a1 & ~b1;     break; /* BIC */
                case 2: r0 = a | b;      r1 = a1 | b1;      break; /* ORR */
                case 3: r0 = a | ~b;     r1 = a1 | ~b1;     break; /* ORN */
                case 4: r0 = a ^ b;      r1 = a1 ^ b1;      break; /* EOR */
                case 5: r0 = (d & ~b) | (a & b); r1 = (d1 & ~b1) | (a1 & b1); break; /* BSL */
                case 6: r0 = (d & b) | (a & ~b); r1 = (d1 & b1) | (a1 & ~b1); break; /* BIT */
                default: r0 = (d & ~b) | (a & b); r1 = (d1 & ~b1) | (a1 & b1); break; /* BIF */
            }
            c->v[Rd][0] = r0; c->v[Rd][1] = Q ? r1 : 0;
            return;
        } else if (opcode == 0x10u || opcode == 0x11u || opcode == 0x13u) {
            /* 0x10 ADD/SUB, 0x11 CMEQ/CMTST, 0x13 MUL. */
            unsigned lanes = VLanes(size, Q);
            uint64_t r0 = 0, r1 = 0;
            for (unsigned i = 0; i < lanes; ++i) {
                uint64_t ea = VGetElem(c, Rn, size, i);
                uint64_t eb = VGetElem(c, Rm, size, i), er;
                if (opcode == 0x10u)      er = U ? (ea - eb) : (ea + eb);
                else if (opcode == 0x13u) er = ea * eb;
                else /* 0x11 */           er = (U ? (ea == eb) : ((ea & eb) != 0)) ? emask : 0;
                er &= emask;
                unsigned off = i * ebits;
                if (off < 64) r0 |= er << off; else r1 |= er << (off - 64);
            }
            c->v[Rd][0] = r0; c->v[Rd][1] = r1;
            return;
        }
    }

    /* AdvSIMD modified immediate: MOVI/MVNI/ORR/BIC/FMOV.
     * 0 Q op 01111 00000 abc cmode 01 defgh Rd  (bits[28:24]=01111,[11:10]=01). */
    if (Field(insn, 31, 31) == 0 && Field(insn, 28, 24) == 0x0F &&
        Field(insn, 11, 10) == 0x01) {
        unsigned op = Field(insn, 29, 29);
        unsigned cmode = Field(insn, 15, 12);
        unsigned imm8 = (Field(insn, 18, 16) << 5) | Field(insn, 9, 5);
        unsigned Rd = Field(insn, 4, 0);
        unsigned kind; /* 0 MOVI, 1 MVNI, 2 ORR, 3 BIC, 4 FMOV */
        uint64_t imm = AdvSimdExpandImmBits(op, cmode, imm8);
        uint64_t result;
        if (cmode < 12)       kind = ((cmode & 1u) == 0) ? (op ? 1u : 0u)
                                                         : (op ? 3u : 2u);
        else if (cmode < 14)  kind = op ? 1u : 0u;
        else if (cmode == 14) kind = 0u;
        else                  kind = op ? 4u : 0u;
        if (kind == 4u) {                 /* FMOV Dd, #imm (scalar) */
            c->v[Rd][0] = imm; c->v[Rd][1] = 0;
            return;
        }
        if (kind == 0u)      result = imm;
        else if (kind == 1u) result = ~imm;
        else if (kind == 2u) result = c->v[Rd][0] | imm;
        else                 result = c->v[Rd][0] & ~imm;
        c->v[Rd][0] = result;
        c->v[Rd][1] = Q ? result : 0;
        return;
    }

    /* Scalar FP data-processing (1/2 source), compare, FP<->int conversion.
     * Shared: bits[31:24]=0x1E, bit21=1; type = bits[23:22]. */
    if ((insn & 0xFF000000u) == 0x1E000000u && Field(insn, 21, 21) == 1u &&
        Field(insn, 23, 22) != 2u) {
        unsigned type = Field(insn, 23, 22);
        unsigned Rn = Field(insn, 9, 5), Rd = Field(insn, 4, 0);

        if (Field(insn, 11, 10) == 0x2u) {            /* 2-source */
            unsigned Rm = Field(insn, 20, 16), op = Field(insn, 15, 12);
            if (type == 0) {
                float a = FBitsToF32(c->v[Rn][0]), b = FBitsToF32(c->v[Rm][0]), r;
                switch (op) {
                    case 0x0: r = a * b; break;
                    case 0x1: r = a / b; break;
                    case 0x2: r = a + b; break;
                    case 0x3: r = a - b; break;
                    case 0x4: r = FMaxNum(a, b); break;
                    case 0x5: r = FMinNum(a, b); break;
                    case 0x6: r = FMaxNum(a, b); break;
                    case 0x7: r = FMinNum(a, b); break;
                    case 0x8: r = -(a * b); break;
                    default: Unimpl(c, pc); return;
                }
                c->v[Rd][0] = F32ToFBits(r); c->v[Rd][1] = 0;
                return;
            } else if (type == 1) {
                double a = FBitsToF64(c->v[Rn][0]), b = FBitsToF64(c->v[Rm][0]), r;
                switch (op) {
                    case 0x0: r = a * b; break;
                    case 0x1: r = a / b; break;
                    case 0x2: r = a + b; break;
                    case 0x3: r = a - b; break;
                    case 0x4: r = DMaxNum(a, b); break;
                    case 0x5: r = DMinNum(a, b); break;
                    case 0x6: r = DMaxNum(a, b); break;
                    case 0x7: r = DMinNum(a, b); break;
                    case 0x8: r = -(a * b); break;
                    default: Unimpl(c, pc); return;
                }
                c->v[Rd][0] = F64ToFBits(r); c->v[Rd][1] = 0;
                return;
            }
        } else if (Field(insn, 14, 10) == 0x10u) {    /* 1-source */
            unsigned op2 = Field(insn, 20, 15);
            if (type == 0) {
                float a = FBitsToF32(c->v[Rn][0]), r;
                switch (op2) {
                    case 0x00: r = a; break;                          /* FMOV */
                    case 0x01: r = (a < 0) ? -a : a; break;           /* FABS */
                    case 0x02: r = -a; break;                         /* FNEG */
                    default: Unimpl(c, pc); return;
                }
                c->v[Rd][0] = F32ToFBits(r); c->v[Rd][1] = 0;
                return;
            } else if (type == 1) {
                double a = FBitsToF64(c->v[Rn][0]), r;
                switch (op2) {
                    case 0x00: r = a; break;
                    case 0x01: r = (a < 0) ? -a : a; break;
                    case 0x02: r = -a; break;
                    default: Unimpl(c, pc); return;
                }
                c->v[Rd][0] = F64ToFBits(r); c->v[Rd][1] = 0;
                return;
            }
        } else if (Field(insn, 15, 10) == 0x08u) {    /* FCMP / FCMPE */
            unsigned op2 = Field(insn, 4, 0);
            unsigned zero = (op2 >> 3) & 1u;
            unsigned Rm = Field(insn, 20, 16);
            int n = 0, z = 0, cc = 0, vv = 0;
            if (type == 0) {
                float a = FBitsToF32(c->v[Rn][0]);
                float b = zero ? 0.0f : FBitsToF32(c->v[Rm][0]);
                if (a != a || (!zero && b != b)) { cc = 1; vv = 1; }
                else if (a == b) { z = 1; cc = 1; }
                else if (a < b) { n = 1; }
                else { cc = 1; }
            } else if (type == 1) {
                double a = FBitsToF64(c->v[Rn][0]);
                double b = zero ? 0.0 : FBitsToF64(c->v[Rm][0]);
                if (a != a || (!zero && b != b)) { cc = 1; vv = 1; }
                else if (a == b) { z = 1; cc = 1; }
                else if (a < b) { n = 1; }
                else { cc = 1; }
            } else { Unimpl(c, pc); return; }
            c->nzcv = (n ? A64_NZCV_N : 0) | (z ? A64_NZCV_Z : 0) |
                      (cc ? A64_NZCV_C : 0) | (vv ? A64_NZCV_V : 0);
            return;
        } else if (Field(insn, 15, 10) == 0x00u) {    /* FP<->int conversion */
            unsigned rmode = Field(insn, 20, 19), opc = Field(insn, 18, 16);
            unsigned sf = Field(insn, 31, 31);
            if (opc == 0u || opc == 1u) {             /* FCVTZS / FCVTZU */
                (void)rmode;                          /* truncation (toward zero) */
                if (type == 0) {
                    float a = FBitsToF32(c->v[Rn][0]);
                    if (opc == 0u) { int64_t r = (int64_t)a;
                        WriteX(c, Rd, sf ? (uint64_t)r : (uint64_t)(int64_t)(int32_t)r); }
                    else { uint64_t r = (uint64_t)a;
                        WriteX(c, Rd, sf ? r : (uint64_t)(uint32_t)r); }
                } else if (type == 1) {
                    double a = FBitsToF64(c->v[Rn][0]);
                    if (opc == 0u) { int64_t r = (int64_t)a;
                        WriteX(c, Rd, sf ? (uint64_t)r : (uint64_t)(int64_t)(int32_t)r); }
                    else { uint64_t r = (uint64_t)a;
                        WriteX(c, Rd, sf ? r : (uint64_t)(uint32_t)r); }
                } else { Unimpl(c, pc); return; }
                return;
            } else if (opc == 2u || opc == 3u) {      /* SCVTF / UCVTF */
                uint64_t src = ReadX(c, Rn);
                if (type == 0) {
                    float r = (opc == 2u)
                        ? (float)(sf ? (int64_t)src : (int64_t)(int32_t)(uint32_t)src)
                        : (float)(sf ? src : (uint64_t)(uint32_t)src);
                    c->v[Rd][0] = F32ToFBits(r); c->v[Rd][1] = 0;
                } else if (type == 1) {
                    double r = (opc == 2u)
                        ? (double)(sf ? (int64_t)src : (int64_t)(int32_t)(uint32_t)src)
                        : (double)(sf ? src : (uint64_t)(uint32_t)src);
                    c->v[Rd][0] = F64ToFBits(r); c->v[Rd][1] = 0;
                } else { Unimpl(c, pc); return; }
                return;
            }
        }
    }

    Unimpl(c, pc);
}

static void DoLoadStore(A64 *c, uint32_t insn, uint64_t pc) {
    uint32_t op = Field(insn, 29, 24);
    if (op == 0x08) { DoExclusive(c, insn, pc); return; }
    if (op == 0x18 || op == 0x19 || op == 0x1C || op == 0x1D) { DoLiteral(c, insn, pc); return; }
    if ((op >= 0x28 && op <= 0x2B) || (op >= 0x2C && op <= 0x2F)) { DoPair(c, insn, pc); return; }
    if (op == 0x38) { DoRegForm(c, insn, pc); return; }
    if (op == 0x39) { DoRegUnsignedImm(c, insn, pc); return; }
    if (op == 0x3C) { DoSimdRegForm(c, insn, pc); return; }
    if (op == 0x3D) { DoSimdUnsignedImm(c, insn, pc); return; }
    Undef(c, pc);
}

/* ================== branches / exceptions / system ===================== */

static void DoSystem(A64 *c, uint32_t insn, uint64_t pc) {
    (void)pc;
    /* bit31=1, bits30:25 = 110101, bits24:22 = 010 -> 110101010x */
    unsigned L = Field(insn, 21, 21), op0 = Field(insn, 20, 19), op1 = Field(insn, 18, 16);
    unsigned CRn = Field(insn, 15, 12), CRm = Field(insn, 11, 8), op2 = Field(insn, 7, 5);
    unsigned Rt = Field(insn, 4, 0);
    if (L == 0 && op0 == 0 && CRn == 4) { return; } /* MSR immediate (PSTATE) */
    /* Hints + barriers (op0=0, op1=3): NOP/YIELD/WFE/WFI/SEV, DSB/DMB/ISB. */
    if (op0 == 0 && op1 == 3) { return; }
    /* Thread pointers: TPIDR_EL0 (op2=2), TPIDRRO_EL0 (op2=3). */
    if (op0 == 3 && op1 == 3 && CRn == 13 && CRm == 0 && (op2 == 2 || op2 == 3)) {
        uint64_t *reg = (op2 == 2) ? &c->tpidr_el0 : &c->tpidrro_el0;
        if (L) WriteX(c, Rt, *reg);
        else   *reg = ReadX(c, Rt);
        return;
    }
    /* Track a handful of system registers by encoding. */
    unsigned key = ((op0 & 3) << 4) | ((CRn & 0xF) ^ (CRm & 0xF) ^ (op2 & 7));
    key &= 63;
    if (L) { WriteX(c, Rt, c->sysregs[key]); }
    else {
        if (CRn == 2 && op2 == 2) { c->nzcv = (uint32_t)(ReadX(c, Rt) & 0xF0000000u); }
        else c->sysregs[key] = ReadX(c, Rt);
    }
}

static void DoBranchSys(A64 *c, uint32_t insn, uint64_t pc) {
    unsigned b31 = Field(insn, 31, 31);
    unsigned b30_26 = Field(insn, 30, 26);
    /* Unconditional branch (immediate): B / BL */
    if (b30_26 == 0x05) {
        uint64_t imm26 = Field(insn, 25, 0);
        uint64_t target = pc + (SignExtend(imm26, 26) << 2);
        if (b31) WriteX(c, 30, pc + 4); /* BL */
        c->pc = target;
        return;
    }
    /* Conditional branch: B.cond */
    if (Field(insn, 31, 24) == 0x54) {
        unsigned cond = Field(insn, 3, 0);
        uint64_t imm19 = Field(insn, 23, 5);
        if (CondCheck(c->nzcv, cond)) c->pc = pc + (SignExtend(imm19, 19) << 2);
        return;
    }
    /* Compare and branch: CBZ / CBNZ */
    if ((insn & 0x7E000000) == 0x34000000) {
        unsigned op = Field(insn, 24, 24);
        uint64_t imm19 = Field(insn, 23, 5);
        unsigned Rt = Field(insn, 4, 0);
        uint64_t v = b31 ? ReadX(c, Rt) : (uint64_t)ReadW(c, Rt);
        int take = op ? (v != 0) : (v == 0);
        if (take) c->pc = pc + (SignExtend(imm19, 19) << 2);
        return;
    }
    /* Test and branch: TBZ / TBNZ */
    if ((insn & 0x7E000000) == 0x36000000) {
        unsigned op = Field(insn, 24, 24);
        unsigned bitpos = (unsigned)((Field(insn, 31, 31) << 5) | Field(insn, 23, 19));
        uint64_t imm14 = Field(insn, 18, 5);
        unsigned Rt = Field(insn, 4, 0);
        uint64_t v = ReadX(c, Rt);
        int bit = (int)((v >> bitpos) & 1);
        int take = op ? (bit != 0) : (bit == 0);
        if (take) c->pc = pc + (SignExtend(imm14, 14) << 2);
        return;
    }
    /* Exception generation: SVC / HVC / SMC / BRK / HLT */
    if (Field(insn, 31, 24) == 0xD4) {
        unsigned opc = Field(insn, 23, 21), LL = Field(insn, 1, 0);
        c->svc_imm = Field(insn, 20, 5);
        if (opc == 0 && (LL == 1 || LL == 2)) { /* SVC / HVC -> HLE bridge */
            if (c->svc_handler != NULL && c->svc_handler(c->svc_ctx, c, c->svc_imm)) {
                return; /* handled; PC already points past the SVC */
            }
            c->halt = A64_HALT_SVC;
            return;
        }
        if (opc == 1 && LL == 0) { c->halt = A64_HALT_BRK; return; }
        if (opc == 2 && LL == 0) { c->halt = A64_HALT_HLT; return; }
        Undef(c, pc);
        return;
    }
    /* System instructions: 1101010100 ... */
    if (Field(insn, 31, 22) == 0x354) { DoSystem(c, insn, pc); return; }
    /* Unconditional branch (register): BR / BLR / RET / ERET */
    if (Field(insn, 31, 25) == 0x6B) {
        unsigned opc = Field(insn, 24, 21), Rn = Field(insn, 9, 5);
        uint64_t target = ReadX(c, Rn);
        if (opc == 0) { c->pc = target; return; }        /* BR  */
        if (opc == 1) { WriteX(c, 30, pc + 4); c->pc = target; return; } /* BLR */
        if (opc == 2) { c->pc = target; return; }        /* RET */
        if (opc == 4) { Unimpl(c, pc); return; }          /* ERET */
        Undef(c, pc);
        return;
    }
    Undef(c, pc);
}

/* ============================== top level ============================== */

static void Decode(A64 *c, uint32_t insn, uint64_t pc) {
    unsigned op0 = Field(insn, 28, 25);
    switch (op0) {
        case 0x8: case 0x9: DoDPI(c, insn, pc); return;
        case 0x4: case 0x6: case 0xC: case 0xE: DoLoadStore(c, insn, pc); return;
        case 0x5: case 0xD: DoDPR(c, insn, pc); return;
        case 0xA: case 0xB: DoBranchSys(c, insn, pc); return;
        case 0x7: case 0xF: DoSimd(c, insn, pc); return; /* scalar FP/SIMD */
        default: Undef(c, pc); return;
    }
}

void A64_Init(A64 *c, A64MemFn read, A64MemFn write, void *memctx) {
    memset(c, 0, sizeof(*c));
    c->read = read;
    c->write = write;
    c->memctx = memctx;
}

void A64_Reset(A64 *c, uint64_t pc, int reset_regs) {
    if (reset_regs) {
        memset(c->x, 0, sizeof(c->x));
        memset(c->v, 0, sizeof(c->v));
        c->sp = 0;
        c->nzcv = 0;
        c->fpcr = c->fpsr = 0;
        c->tpidr_el0 = c->tpidrro_el0 = 0;
        memset(c->sysregs, 0, sizeof(c->sysregs));
    }
    c->pc = pc;
    c->halt = A64_OK;
    c->fault_addr = 0;
    c->fault_size = 0;
    c->fault_write = 0;
    c->undefined_pc = 0;
    c->svc_imm = 0;
    c->icount = 0;
    c->sp_zero_pc = 0;
    c->sp_prev = 0;
}

void A64_SetSvcHandler(A64 *c, A64SvcFn fn, void *ctx) {
    c->svc_handler = fn;
    c->svc_ctx = ctx;
}

const char *A64_HaltName(A64Halt halt) {
    switch (halt) {
        case A64_OK:                 return "ok";
        case A64_HALT_SVC:           return "svc (unhandled)";
        case A64_HALT_BRK:           return "brk";
        case A64_HALT_HLT:           return "hlt";
        case A64_HALT_UNDEFINED:     return "undefined-encoding";
        case A64_HALT_UNIMPLEMENTED: return "unimplemented-encoding";
        case A64_HALT_MEMFAULT:      return "memory-fault";
        case A64_HALT_STEPLIMIT:     return "step-limit";
        default:                     return "unknown-halt";
    }
}

A64Halt A64_Step(A64 *c) {
    uint64_t insn_le = 0;
    uint64_t prev_sp;
    if (!LoadLE(c, c->pc, &insn_le, 4)) return c->halt;
    uint32_t insn = (uint32_t)insn_le;
    uint64_t pc = c->pc;
    c->pc = pc + 4;
    c->halt = A64_OK;
    prev_sp = c->sp;
    if (c->on_insn && !c->on_insn(c->insn_ctx, c, pc, insn)) {
        c->halt = A64_HALT_STEPLIMIT;
        return c->halt;
    }
    { /* TEMP debug: A64_TRACE=lo-hi (hex) logs each step in range. */
        static int inited = 0; static unsigned long long lo = 0, hi = 0;
        if (!inited) {
            const char *e = getenv("A64_TRACE");
            if (e) sscanf(e, "%llx-%llx", &lo, &hi);
            inited = 1;
        }
        if (lo && pc >= lo && pc <= hi)
            fprintf(stderr, "pc=0x%llx insn=0x%08x x9=0x%llx x10=0x%llx x11=0x%llx x12=0x%llx x13=0x%llx x14=0x%llx\n",
                    (unsigned long long)pc, insn,
                    (unsigned long long)c->x[9], (unsigned long long)c->x[10],
                    (unsigned long long)c->x[11], (unsigned long long)c->x[12],
                    (unsigned long long)c->x[13], (unsigned long long)c->x[14]);
    }
    { /* TEMP debug: EDEN_TRAP=addr[,addr...] (hex) logs regs on hit. */
        static int tinit = 0; static unsigned long long trap[16]; static int ntrap = 0;
        if (!tinit) {
            const char *e = getenv("EDEN_TRAP");
            if (e) {
                const char *p = e;
                while (*p && ntrap < 16) {
                    trap[ntrap++] = strtoull(p, (char **)&p, 16);
                    while (*p == ',' || *p == ' ') ++p;
                }
            }
            tinit = 1;
        }
        {
            int ti;
            for (ti = 0; ti < ntrap; ++ti)
                if (pc == trap[ti])
                    fprintf(stderr, "TRAP pc=0x%llx x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx x8=0x%llx x9=0x%llx x19=0x%llx x29=0x%llx x30=0x%llx sp=0x%llx\n",
                            (unsigned long long)pc,
                            (unsigned long long)c->x[0], (unsigned long long)c->x[1],
                            (unsigned long long)c->x[2], (unsigned long long)c->x[3],
                            (unsigned long long)c->x[8], (unsigned long long)c->x[9],
                            (unsigned long long)c->x[19], (unsigned long long)c->x[29],
                            (unsigned long long)c->x[30], (unsigned long long)c->sp);
        }
    }
    { /* TEMP debug: EDEN_LOG_PC=addr[,addr...] (hex) logs regs and continues. */
        static int linit = 0; static unsigned long long lpc[16]; static int nlpc = 0;
        if (!linit) {
            const char *e = getenv("EDEN_LOG_PC");
            if (e) {
                const char *p = e;
                while (*p && nlpc < 16) {
                    lpc[nlpc++] = strtoull(p, (char **)&p, 16);
                    while (*p == ',' || *p == ' ') ++p;
                }
            }
            linit = 1;
        }
        {
            int li;
            for (li = 0; li < nlpc; ++li)
                if (pc == lpc[li])
                    fprintf(stderr, "LOGPC pc=0x%llx x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx x8=0x%llx x9=0x%llx x10=0x%llx x11=0x%llx x19=0x%llx x20=0x%llx x21=0x%llx x22=0x%llx x29=0x%llx x30=0x%llx sp=0x%llx\n",
                            (unsigned long long)pc,
                            (unsigned long long)c->x[0], (unsigned long long)c->x[1],
                            (unsigned long long)c->x[2], (unsigned long long)c->x[3],
                            (unsigned long long)c->x[8], (unsigned long long)c->x[9],
                            (unsigned long long)c->x[10], (unsigned long long)c->x[11],
                            (unsigned long long)c->x[19],
                            (unsigned long long)c->x[20], (unsigned long long)c->x[21],
                            (unsigned long long)c->x[22],
                            (unsigned long long)c->x[29], (unsigned long long)c->x[30],
                            (unsigned long long)c->sp);
        }
    }
    Decode(c, insn, pc);
    if (c->halt == A64_OK) ++c->icount;
    /* Debug: remember the first instruction that drove sp to (near) zero. */
    if (c->sp_zero_pc == 0 && c->sp != prev_sp && c->sp < 0x1000)
        c->sp_zero_pc = pc;
    return c->halt;
}

A64Halt A64_Run(A64 *c, uint64_t max_instructions) {
    for (uint64_t i = 0; i < max_instructions; ++i) {
        A64Halt h = A64_Step(c);
        if (h != A64_OK) return h;
    }
    return A64_OK;
}
