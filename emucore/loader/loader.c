/* loader.c: load an NSO image into guest memory. */
#include "loader.h"

#include <string.h>

const char *Load_ResultName(LoadResult r) {
    switch (r) {
        case LOAD_OK:             return "ok";
        case LOAD_ERR_NSO:        return "invalid NSO";
        case LOAD_ERR_RANGE:      return "segment range error";
        case LOAD_ERR_DECOMPRESS: return "lz4 decompression failed";
        case LOAD_ERR_MEM:        return "guest memory fault";
        default:                  return "unknown";
    }
}

LoadResult Nso_LoadIntoGuest(const Nso *n, const void *nso_data, uint64_t nso_size,
                             A64GuestMem *mem, LoadedImage *out) {
    const uint8_t *src = (const uint8_t *)nso_data;
    uint64_t base = 0, end = 0, mapped = 0;
    int have_base = 0;
    unsigned i;

    if (out) { out->entry = out->image_base = out->image_end = out->mapped = 0; }
    if (n == NULL || !n->valid) return LOAD_ERR_NSO;

    for (i = 0; i < 3; ++i) {
        const NsoSegment *s = &n->seg[i];
        uint8_t *host;
        uint64_t seg_end;

        if (s->size == 0) continue;

        if ((uint64_t)s->offset > nso_size || (uint64_t)s->compressed_size > nso_size - s->offset)
            return LOAD_ERR_RANGE;
        if ((uint64_t)s->size > nso_size - s->offset) {
            /* only meaningful for compressed input; raw needs exact fit */
            if (!s->compressed) return LOAD_ERR_RANGE;
        }

        host = A64GuestMem_Host(mem, s->location, s->size);
        if (host == NULL) return LOAD_ERR_MEM;

        if (s->compressed) {
            uint32_t produced = 0;
            if (!Nso_Lz4Decompress(src + s->offset, s->compressed_size, host, s->size, &produced))
                return LOAD_ERR_DECOMPRESS;
            if (produced != s->size) return LOAD_ERR_DECOMPRESS;
        } else {
            if (s->size > s->compressed_size) return LOAD_ERR_RANGE;
            memcpy(host, src + s->offset, s->size);
        }

        seg_end = (uint64_t)s->location + s->size;
        if (s->bss_size) {
            uint8_t *z = A64GuestMem_Host(mem, seg_end, s->bss_size);
            if (z == NULL) return LOAD_ERR_MEM;
            memset(z, 0, s->bss_size);
            seg_end += s->bss_size;
        }

        if (!have_base) { base = s->location; have_base = 1; }
        if (seg_end > end) end = seg_end;
        mapped += s->size;
    }

    if (out) {
        out->entry = n->entry;
        out->image_base = base;
        out->image_end = end;
        out->mapped = mapped;
    }
    return LOAD_OK;
}
