/* a64.h: portable AArch64 (ARMv8-A) CPU interpreter.
 *
 * Written for Eden's Xbox 360 target (PowerPC Xenon / libXenon): plain C99,
 * no libc dependencies beyond <stdint.h>/<stddef.h>, so the exact same code
 * runs in host unit tests and in the bare-metal libXenon build.
 *
 * Every implemented instruction performs real work on real register/flag state
 * and real guest memory. Any encoding that is not implemented halts with a
 * distinct reason, so a caller can never mistake "not supported yet" for
 * "executed successfully".
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Guest memory access. Return 1 on success, 0 on fault. */
typedef int (*A64MemFn)(void *ctx, uint64_t addr, void *buf, size_t len);

struct A64;

/* SVC/HLE bridge. Called when the guest executes SVC/HVC. The handler runs
 * with the CPU PC already advanced past the SVC, so it may read/modify
 * registers (the HLE ABI passes the call number in X8 and args in X0..X7) and
 * return normally to continue execution. Return 1 if the call was handled,
 * 0 to leave it unhandled (the CPU then halts with A64_HALT_SVC). */
typedef int (*A64SvcFn)(void *ctx, struct A64 *cpu, uint64_t imm);

typedef enum {
    A64_OK = 0,             /* ran the requested instruction count        */
    A64_HALT_SVC,           /* SVC executed (guest syscall)               */
    A64_HALT_BRK,           /* BRK executed                               */
    A64_HALT_HLT,           /* HLT executed                               */
    A64_HALT_UNDEFINED,     /* unrecognised encoding                      */
    A64_HALT_UNIMPLEMENTED, /* recognised but not implemented yet         */
    A64_HALT_MEMFAULT,      /* guest memory access failed                 */
    A64_HALT_STEPLIMIT,     /* A64_Run hit its instruction budget         */
} A64Halt;

#define A64_NZCV_N (1u << 31)
#define A64_NZCV_Z (1u << 30)
#define A64_NZCV_C (1u << 29)
#define A64_NZCV_V (1u << 28)

typedef struct A64 {
    uint64_t x[31];         /* X0..X30                               */
    uint64_t sp;
    uint64_t pc;

    uint32_t nzcv;
    uint32_t fpcr, fpsr;

    A64MemFn read;
    A64MemFn write;
    void    *memctx;

    /* A small scratch bank for system registers we track by encoding
     * (NZCV/TPIDR_EL0/FPCR/... map to fixed slots). Not architecturally
     * complete; unmapped system registers read as 0 and writes are ignored. */
    uint64_t sysregs[64];

    A64Halt  halt;
    uint64_t fault_addr;    /* address of the last memory fault        */
    uint64_t fault_size;    /* size in bytes of the last failed access */
    int      fault_write;   /* 1 if the last failed access was a write */
    uint64_t undefined_pc;  /* PC of the last bad instruction          */
    uint64_t svc_imm;       /* immediate of the last SVC/BRK/HLT       */

    /* Instruction count executed since the last reset. */
    uint64_t icount;

    /* Optional per-instruction hook. Return 0 to stop (STEPLIMIT). */
    int (*on_insn)(void *ctx, struct A64 *cpu, uint64_t pc, uint32_t insn);
    void *insn_ctx;

    /* Optional SVC/HLE handler (see A64SvcFn). */
    A64SvcFn svc_handler;
    void    *svc_ctx;
} A64;

void A64_Init(A64 *cpu, A64MemFn read, A64MemFn write, void *memctx);
void A64_Reset(A64 *cpu, uint64_t pc, int reset_regs);

/* Install/clear the SVC/HLE bridge (fn may be NULL). */
void A64_SetSvcHandler(A64 *cpu, A64SvcFn fn, void *ctx);

/* Stable, human-readable name for a halt reason (never NULL). */
const char *A64_HaltName(A64Halt halt);

/* Register access helpers (also usable by HLE handlers). */
uint64_t A64_GetX(const A64 *cpu, unsigned n);
void A64_SetX(A64 *cpu, unsigned n, uint64_t v);

A64Halt A64_Step(A64 *cpu);
A64Halt A64_Run(A64 *cpu, uint64_t max_instructions);

/* Direct register helpers (n in 0..30; 31 is XZR for x, SP for sp). */
uint64_t A64_GetX(const A64 *cpu, unsigned n);
void     A64_SetX(A64 *cpu, unsigned n, uint64_t v);

#ifdef __cplusplus
}
#endif
