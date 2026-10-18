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
    int      fault_oom;   /* the last fault was frame-pool exhaustion        */

    /* Access counters (successful accesses only). */
    uint64_t reads;
    uint64_t writes;

    /* Optional sparse, demand-paged window that sits above the flat buffer.
     * The window covers [sp_base, sp_base + sp_size) and is divided into
     * fixed-size pages; a page is backed by a frame from a bounded host pool
     * the first time it is written, so a guest may reserve gigabytes of address
     * space while only its touched pages consume real memory. sp_frames and
     * sp_map are caller-owned (see A64GuestMem_InitSparse). */
    uint64_t sp_base;     /* guest address of sp_page 0 (0 = no window)      */
    uint64_t sp_size;     /* window size in bytes                            */
    uint64_t sp_page_size;/* bytes per page/frame (power of two)             */
    uint8_t *sp_frames;   /* host frame pool (frame i at i*sp_page_size)     */
    uint64_t sp_frame_cap;/* frames available in the pool                    */
    uint64_t sp_frame_used;/* frames handed out so far                       */
    uint32_t *sp_map;     /* map[page] = frame index + 1, or 0 if unmapped   */
    uint64_t sp_page_cap; /* capacity of sp_map                              */
    uint64_t sp_committed;/* bytes of the window actually backed             */
    uint64_t sp_oom;      /* number of accesses that failed for want of a    */
                          /* frame (the pool was exhausted)                  */
} A64GuestMem;

/* Attach a host buffer. `guest_base` is the guest address of buf[0]. */
void A64GuestMem_Init(A64GuestMem *m, void *buf, uint64_t size, uint64_t guest_base);

/* Enable a sparse, demand-paged window above the flat buffer. `frames` is a
 * host pool of `frame_bytes` bytes given out one page at a time; `map` holds up
 * to `map_pages` page->frame entries. All three are caller-owned and must stay
 * valid for the lifetime of `m`. Reads of a page never written return zero;
 * writes allocate a frame (up to the pool) and fault with `fault_oom` set when
 * the pool is exhausted. */
void A64GuestMem_InitSparse(A64GuestMem *m, void *map, uint64_t map_pages,
                            void *frames, uint64_t frame_bytes,
                            uint64_t page_size);

/* Back the window [base, base+size) in the sparse area. Both must be page
 * aligned and `size` must fit in the map supplied to A64GuestMem_InitSparse.
 * Returns 1 on success, 0 if the area is too small or misaligned. */
int A64GuestMem_MapSparse(A64GuestMem *m, uint64_t base, uint64_t size);

/* Bytes of the sparse window currently backed by frames. */
uint64_t A64GuestMem_SparseCommitted(const A64GuestMem *m);

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
