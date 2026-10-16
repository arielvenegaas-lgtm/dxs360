/* kernel.h: Horizon OS kernel-call (SVC) HLE handlers.
 *
 * Real Switch user code issues `svc #N` with N the syscall number and
 * arguments in X0..X7; the result is left in X0. This module implements the
 * subset of kernel calls that a booting runtime (rtld, then the SDK) actually
 * needs, over a caller-supplied memory map. No call pretends to succeed: an
 * unimplemented number is left unhandled so the interpreter halts and the stop
 * is reported by number.
 *
 * The memory map is described as a flat list of regions (the modules' mapped
 * segments plus the process stack). QueryMemory answers from it.
 */
#pragma once

#include <stdint.h>
#include "hle.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HLEK_MAX_REGIONS 80

/* MemoryType values (Atmosphere/Horizon subset). */
#define HLEK_MEM_UNMAPPED   0x00u
#define HLEK_MEM_IO         0x01u
#define HLEK_MEM_NORMAL     0x02u
#define HLEK_MEM_CODE_STATIC 0x03u
#define HLEK_MEM_CODE_MUT  0x04u
#define HLEK_MEM_HEAP       0x05u
#define HLEK_MEM_STACK      0x0Au
#define HLEK_MEM_CODE_RO    0x13u
#define HLEK_MEM_CODE_WR    0x14u

/* Permission bits as Horizon reports them. */
#define HLEK_PERM_R 1u
#define HLEK_PERM_W 2u
#define HLEK_PERM_X 4u

typedef struct HleKernelRegion {
    uint64_t base;
    uint64_t size;
    uint32_t type;
    uint32_t perm;
} HleKernelRegion;

/* Optional sink for OutputDebugString (and other diagnostics). The string is
 * NUL-terminated; the platform decides where it goes. */
typedef void (*HleKernelLog)(void *ctx, const char *s);

typedef struct HleKernel {
    HleKernelRegion regions[HLEK_MAX_REGIONS];
    int      count;

    uint64_t span_end;   /* top of the address space tracked (exclusive)   */
    uint64_t heap_base;  /* set by SetHeapSize                             */
    uint64_t heap_size;
    uint64_t tick;       /* monotonic counter for GetSystemTick            */
    int      exited;     /* ExitProcess was called                         */

    HleKernelLog log;
    void        *log_ctx;

    /* Counters for honest reporting. */
    uint64_t query_count;
    uint64_t unknown_count;
    uint64_t last_unknown;
} HleKernel;

void HleKernel_Init(HleKernel *k);
void HleKernel_SetSpanEnd(HleKernel *k, uint64_t end);
void HleKernel_SetLog(HleKernel *k, HleKernelLog fn, void *ctx);
int  HleKernel_AddRegion(HleKernel *k, uint64_t base, uint64_t size,
                         uint32_t type, uint32_t perm);

/* Register this kernel's handlers into an HLE registry. */
void HleKernel_Register(HleState *h, HleKernel *k);

/* Human-readable name of a syscall number (for diagnostics; never NULL). */
const char *HleKernel_SvcName(uint64_t svc);

#ifdef __cplusplus
}
#endif
