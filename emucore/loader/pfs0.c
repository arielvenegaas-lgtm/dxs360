/* pfs0.c: portable, bounds-checked PFS0 parser. */
#include "pfs0.h"

#include <string.h>

static uint32_t RdU32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t RdU64(const uint8_t *p) {
    return (uint64_t)RdU32(p) | ((uint64_t)RdU32(p + 4) << 32);
}

const char *Pfs0_ResultName(Pfs0Result r) {
    switch (r) {
        case PFS0_OK:            return "ok";
        case PFS0_ERR_TOO_SMALL: return "file too small";
        case PFS0_ERR_BAD_MAGIC: return "bad magic (not PFS0)";
        case PFS0_ERR_TOO_MANY:  return "too many files";
        case PFS0_ERR_RANGE:     return "entry out of range";
        default:                 return "unknown";
    }
}

Pfs0Result Pfs0_Parse(Pfs0 *p, const void *data, uint64_t size) {
    const uint8_t *d = (const uint8_t *)data;
    uint32_t count, strtab;
    uint64_t header_size;
    uint32_t i;
    int abs_ok = 1, rel_ok = 1;

    memset(p, 0, sizeof(*p));
    p->data = d;
    p->size = size;

    if (size < 0x10) return PFS0_ERR_TOO_SMALL;
    if (d[0] != 'P' || d[1] != 'F' || d[2] != 'S' || d[3] != '0') return PFS0_ERR_BAD_MAGIC;

    count  = RdU32(d + 4);
    strtab = RdU32(d + 8);
    if (count > PFS0_MAX_FILES) return PFS0_ERR_TOO_MANY;

    header_size = (uint64_t)0x10 + (uint64_t)count * 0x18u + (uint64_t)strtab;
    if (header_size > size) return PFS0_ERR_TOO_SMALL;

    p->count = count;
    p->strtab_size = strtab;
    p->header_size = header_size;

    /* Decide whether entry offsets are absolute (canonical) or header-relative. */
    for (i = 0; i < count; ++i) {
        const uint8_t *e = d + 0x10 + (uint64_t)i * 0x18u;
        uint64_t off = RdU64(e);
        uint64_t sz = RdU64(e + 8);
        uint64_t abs = header_size + off;

        if (off < header_size || off > size || sz > size - off) abs_ok = 0;
        if (off > size || abs < off /*overflow*/ || abs > size || sz > size - abs) rel_ok = 0;
    }
    if (abs_ok) {
        p->relative = 0;
    } else if (rel_ok) {
        p->relative = 1;
    } else {
        return PFS0_ERR_RANGE;
    }

    /* Resolve absolute offsets and validate that every name is NUL-terminated
     * within the string table. */
    for (i = 0; i < count; ++i) {
        const uint8_t *e = d + 0x10 + (uint64_t)i * 0x18u;
        uint64_t off = RdU64(e);
        uint32_t name_off = RdU32(e + 16);

        p->entries[i].offset = p->relative ? header_size + off : off;
        p->entries[i].size = RdU64(e + 8);
        p->entries[i].name_off = name_off;

        if (strtab == 0) {
            if (name_off != 0) return PFS0_ERR_RANGE;
        } else {
            const uint8_t *st = d + (header_size - strtab);
            const uint8_t *end = st + strtab;
            const uint8_t *q;
            int terminated = 0;
            if (name_off >= strtab) return PFS0_ERR_RANGE;
            for (q = st + name_off; q < end; ++q) {
                if (*q == 0) { terminated = 1; break; }
            }
            if (!terminated) return PFS0_ERR_RANGE;
        }
    }

    return PFS0_OK;
}

const char *Pfs0_Name(const Pfs0 *p, uint32_t i) {
    const char *strtab;
    if (i >= p->count) return "";
    strtab = (const char *)(p->data + (p->header_size - p->strtab_size));
    if (p->entries[i].name_off >= p->strtab_size && p->strtab_size != 0) return "";
    return strtab + p->entries[i].name_off;
}

const uint8_t *Pfs0_Data(const Pfs0 *p, uint32_t i, uint64_t *out_size) {
    if (i >= p->count) return NULL;
    if (out_size) *out_size = p->entries[i].size;
    return p->data + p->entries[i].offset;
}

int Pfs0_Find(const Pfs0 *p, const char *name) {
    uint32_t i;
    for (i = 0; i < p->count; ++i) {
        if (strcmp(Pfs0_Name(p, i), name) == 0) return (int)i;
    }
    return -1;
}
