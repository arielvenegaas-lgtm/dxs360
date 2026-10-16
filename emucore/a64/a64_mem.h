/* a64_mem.h: safe guest physical memory for the A64 interpreter.
 *
 * A flat, bounds-checked window over a caller-owned host buffer. Every access
 * is validated before it touches host memory, and the first out-of-range
 * access is recorded so the caller can report exactly which address/size/direction
 * faulted instead of crashing. This is the memory backend used by the dxs360
 * loader and by the host tests; it has no libc or OS dependency.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "a64.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct A64GuestMem {
    uint8_t *base;        /* host buffer                                     */
    uint64_t size;        /* size of the buffer in bytes                     */
    uint64_t guest_base;  /* guest address that maps to base[0]              */

    /* Fault bookkeeping (updated on a failed access). */
    uint64_t fault_addr;
    uint64_t fault_size;
    int      fault_write;
    uint64_t faults;

    /* Access counters (successful accesses only). */
    uint64_t reads;
    uint64_t writes;
} A64GuestMem;

/* Attach a host buffer. `guest_base` is the guest address of buf[0]. */
void A64GuestMem_Init(A64GuestMem *m, void *buf, uint64_t size, uint64_t guest_base);

/* Translate a guest range to a host pointer; NULL (and a recorded fault) if
 * the range is not fully inside the window. */
uint8_t *A64GuestMem_Host(A64GuestMem *m, uint64_t addr, uint64_t len);

/* A64MemFn-compatible callbacks. `out`/`in` must be non-NULL. */
int A64GuestMem_Read(void *ctx, uint64_t addr, void *out, size_t len);
int A64GuestMem_Write(void *ctx, uint64_t addr, void *in, size_t len);

/* A64_Init(&cpu, A64GuestMem_Read, A64GuestMem_Write, mem). */
void A64GuestMem_Attach(A64GuestMem *m, A64 *cpu);

#ifdef __cplusplus
}
#endif
