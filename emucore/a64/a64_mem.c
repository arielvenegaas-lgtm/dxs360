/* a64_mem.c: safe guest physical memory for the A64 interpreter. */
#include "a64_mem.h"

#include <string.h>

void A64GuestMem_Init(A64GuestMem *m, void *buf, uint64_t size, uint64_t guest_base) {
    memset(m, 0, sizeof(*m));
    m->base = (uint8_t *)buf;
    m->size = size;
    m->guest_base = guest_base;
}

uint8_t *A64GuestMem_Host(A64GuestMem *m, uint64_t addr, uint64_t len) {
    uint64_t off;
    if (addr < m->guest_base) {
        m->fault_addr = addr;
        m->fault_size = len;
        ++m->faults;
        return NULL;
    }
    off = addr - m->guest_base;
    if (off > m->size || len > m->size - off) {
        m->fault_addr = addr;
        m->fault_size = len;
        ++m->faults;
        return NULL;
    }
    return m->base + off;
}

int A64GuestMem_Read(void *ctx, uint64_t addr, void *out, size_t len) {
    A64GuestMem *m = (A64GuestMem *)ctx;
    uint8_t *host;
    if (out == NULL) return 0;
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
    host = A64GuestMem_Host(m, addr, (uint64_t)len);
    if (host == NULL) { m->fault_write = 1; return 0; }
    memcpy(host, in, len);
    ++m->writes;
    return 1;
}

void A64GuestMem_Attach(A64GuestMem *m, A64 *cpu) {
    A64_Init(cpu, A64GuestMem_Read, A64GuestMem_Write, m);
}
