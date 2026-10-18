/* a64_mem.c: safe guest physical memory for the A64 interpreter. */
#include "a64_mem.h"

#include <string.h>

void A64GuestMem_Init(A64GuestMem *m, void *buf, uint64_t size, uint64_t guest_base) {
    memset(m, 0, sizeof(*m));
    m->base = (uint8_t *)buf;
    m->size = size;
    m->guest_base = guest_base;
}

void A64GuestMem_InitSparse(A64GuestMem *m, void *map, uint64_t map_pages,
                            void *frames, uint64_t frame_bytes,
                            uint64_t page_size) {
    m->sp_map = (uint32_t *)map;
    m->sp_page_cap = map_pages;
    m->sp_frames = (uint8_t *)frames;
    m->sp_page_size = page_size;
    m->sp_frame_cap = page_size ? (frame_bytes / page_size) : 0;
    m->sp_base = 0;
    m->sp_size = 0;
    m->sp_frame_used = 0;
    m->sp_committed = 0;
    m->sp_oom = 0;
}

int A64GuestMem_MapSparse(A64GuestMem *m, uint64_t base, uint64_t size) {
    if (m->sp_frames == NULL || m->sp_map == NULL || m->sp_page_size == 0)
        return 0;
    if ((base & (m->sp_page_size - 1)) || (size & (m->sp_page_size - 1)))
        return 0;
    if (size == 0 || size / m->sp_page_size > m->sp_page_cap)
        return 0;
    m->sp_base = base;
    m->sp_size = size;
    return 1;
}

uint64_t A64GuestMem_SparseCommitted(const A64GuestMem *m) {
    return m->sp_committed;
}

/* Resolve `addr` (inside the sparse window) to its host frame, optionally
 * allocating a frame for a page written for the first time. Returns NULL when
 * the page is unmapped (and no allocation was requested) or when the frame pool
 * is exhausted; *oom is set in the latter case. */
static uint8_t *SparseFrame(A64GuestMem *m, uint64_t addr, int alloc, int *oom) {
    uint64_t page = (addr - m->sp_base) / m->sp_page_size;
    uint32_t e;
    if (page >= m->sp_page_cap) { *oom = 0; return NULL; }
    e = m->sp_map[page];
    if (e == 0) {
        uint64_t off;
        if (!alloc) return NULL;
        if (m->sp_frame_used >= m->sp_frame_cap) { *oom = 1; return NULL; }
        e = (uint32_t)(++m->sp_frame_used);   /* 1-based; 0 means unmapped */
        off = (uint64_t)(e - 1) * m->sp_page_size;
        memset(m->sp_frames + off, 0, m->sp_page_size);
        m->sp_map[page] = e;
        m->sp_committed += m->sp_page_size;
    }
    return m->sp_frames + (uint64_t)(e - 1) * m->sp_page_size;
}

static int SparseRead(A64GuestMem *m, uint64_t addr, uint8_t *out, size_t len) {
    while (len) {
        uint64_t off = (addr - m->sp_base) & (m->sp_page_size - 1);
        size_t chunk = (size_t)(m->sp_page_size - off);
        uint8_t *p;
        int oom = 0;
        if (chunk > len) chunk = len;
        p = SparseFrame(m, addr, 0, &oom);
        if (p) memcpy(out, p + off, chunk);
        else memset(out, 0, chunk);          /* never written: reads as zero */
        addr += chunk; out += chunk; len -= chunk;
    }
    return 1;
}

static int SparseWrite(A64GuestMem *m, uint64_t addr, const uint8_t *in, size_t len) {
    while (len) {
        uint64_t off = (addr - m->sp_base) & (m->sp_page_size - 1);
        size_t chunk = (size_t)(m->sp_page_size - off);
        uint8_t *p;
        int oom = 0;
        if (chunk > len) chunk = len;
        p = SparseFrame(m, addr, 1, &oom);
        if (!p) {
            m->fault_addr = addr;
            m->fault_size = chunk;
            m->fault_write = 1;
            m->fault_oom = 1;
            ++m->faults;
            ++m->sp_oom;
            return 0;
        }
        memcpy(p + off, in, chunk);
        addr += chunk; in += chunk; len -= chunk;
    }
    return 1;
}

uint8_t *A64GuestMem_Host(A64GuestMem *m, uint64_t addr, uint64_t len) {
    uint64_t off;
    if (addr < m->guest_base) {
        m->fault_addr = addr;
        m->fault_size = len;
        m->fault_oom = 0;
        ++m->faults;
        return NULL;
    }
    off = addr - m->guest_base;
    if (off > m->size || len > m->size - off) {
        m->fault_addr = addr;
        m->fault_size = len;
        m->fault_oom = 0;
        ++m->faults;
        return NULL;
    }
    return m->base + off;
}

int A64GuestMem_Read(void *ctx, uint64_t addr, void *out, size_t len) {
    A64GuestMem *m = (A64GuestMem *)ctx;
    uint8_t *host;
    if (out == NULL) return 0;
    if (m->sp_size && addr >= m->sp_base && addr - m->sp_base <= m->sp_size &&
        len <= m->sp_size - (addr - m->sp_base)) {
        if (!SparseRead(m, addr, (uint8_t *)out, len)) { m->fault_write = 0; return 0; }
        ++m->reads;
        return 1;
    }
    host = A64GuestMem_Host(m, addr, (uint64_t)len);
    if (host == NULL) { m->fault_write = 0; return 0; }
    memcpy(out, host, len);
    ++m->reads;
    return 1;
}

int A64GuestMem_Write(void *ctx, uint64_t addr, void *in, size_t len) {
    A64GuestMem *m = (A64GuestMem *)ctx;
    uint8_t *host;
    if (in == NULL) return 0;
    if (m->sp_size && addr >= m->sp_base && addr - m->sp_base <= m->sp_size &&
        len <= m->sp_size - (addr - m->sp_base)) {
        if (!SparseWrite(m, addr, (const uint8_t *)in, len)) { m->fault_write = 1; return 0; }
        ++m->writes;
        return 1;
    }
    host = A64GuestMem_Host(m, addr, (uint64_t)len);
    if (host == NULL) { m->fault_write = 1; return 0; }
    memcpy(host, in, len);
    ++m->writes;
    return 1;
}

void A64GuestMem_Attach(A64GuestMem *m, A64 *cpu) {
    A64_Init(cpu, A64GuestMem_Read, A64GuestMem_Write, m);
}
