/* nso.c: portable NSO header parser + LZ4 block decompressor. */
#include "nso.h"

#include <string.h>

static uint32_t RdU32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const char *Nso_ResultName(NsoResult r) {
    switch (r) {
        case NSO_OK:            return "ok";
        case NSO_ERR_TOO_SMALL: return "file too small";
        case NSO_ERR_BAD_MAGIC: return "bad magic (not NSO0)";
        case NSO_ERR_RANGE:     return "segment out of range";
        case NSO_ERR_ZBIC:      return "zbic-compressed (unsupported)";
        default:                return "unknown";
    }
}

NsoResult Nso_Parse(Nso *n, const void *data, uint64_t size) {
    const uint8_t *d = (const uint8_t *)data;

    memset(n, 0, sizeof(*n));
    if (size < NSO_HEADER_SIZE) return NSO_ERR_TOO_SMALL;
    if (d[0] != 'N' || d[1] != 'S' || d[2] != 'O' || d[3] != '0') return NSO_ERR_BAD_MAGIC;

    n->valid = 1;
    n->version = RdU32(d + 4);
    n->flags = RdU32(d + 0x0C);

    if ((n->flags >> 7) & 1u) return NSO_ERR_ZBIC;

    memcpy(n->build_id, d + 0x40, 0x20);

    /* Header +0x1C is the text segment's "align_or_total_size": for a module
     * image it is the size of the MOD0 module-header block that prefixes .text.
     * Executable code begins immediately after it, so the entry point is
     * text_location + module_offset (for retail NSOs this is .text + 0x100).
     * Using text_location alone would execute the MOD0 pointer prologue. */
    n->module_offset = RdU32(d + 0x1C);
    if (n->module_offset > 0x1000) return NSO_ERR_RANGE; /* sanity */

    for (unsigned i2 = 0; i2 < 3; ++i2) {
        const uint8_t *s = d + 0x10 + i2 * 0x10;
        NsoSegment *seg = &n->seg[i2];
        uint32_t csize;
        seg->offset = RdU32(s + 0);
        seg->location = RdU32(s + 4);
        seg->size = RdU32(s + 8);
        seg->bss_size = (i2 == 2) ? RdU32(s + 0xC) : 0; /* alignment otherwise */
        seg->compressed = (int)((n->flags >> i2) & 1u);
        csize = seg->compressed ? RdU32(d + 0x60 + i2 * 4) : seg->size;
        seg->compressed_size = csize;

        if (seg->size == 0) continue;
        if (seg->offset > size || csize > size - seg->offset) return NSO_ERR_RANGE;
    }

    n->valid = 1;
    n->entry = (uint64_t)n->seg[0].location + n->module_offset;
    return NSO_OK;
}

int Nso_Lz4Decompress(const uint8_t *src, uint32_t src_size,
                      uint8_t *dst, uint32_t dst_cap, uint32_t *out_size) {
    const uint8_t *ip = src;
    const uint8_t *iend = src + src_size;
    uint8_t *op = dst;
    uint8_t *oend = dst + dst_cap;

    while (ip < iend) {
        uint8_t token = *ip++;
        size_t lit_len = (size_t)(token >> 4);
        size_t match_len;
        uint32_t offset;

        if (lit_len == 15) {
            uint8_t b;
            do {
                if (ip >= iend) return 0;
                b = *ip++;
                lit_len += b;
            } while (b == 255);
        }
        if ((size_t)(iend - ip) < lit_len) return 0;
        if ((size_t)(oend - op) < lit_len) return 0;
        memcpy(op, ip, lit_len);
        op += lit_len;
        ip += lit_len;

        if (ip >= iend) break; /* last sequence has literals only */

        if ((size_t)(iend - ip) < 2) return 0;
        offset = (uint32_t)ip[0] | ((uint32_t)ip[1] << 8);
        ip += 2;
        if (offset == 0 || (size_t)(op - dst) < offset) return 0;

        match_len = (size_t)(token & 0x0F) + 4;
        if ((token & 0x0F) == 15) {
            uint8_t b;
            do {
                if (ip >= iend) return 0;
                b = *ip++;
                match_len += b;
            } while (b == 255);
        }
        if ((size_t)(oend - op) < match_len) return 0;
        {
            const uint8_t *match = op - offset;
            size_t k;
            for (k = 0; k < match_len; ++k) op[k] = match[k];
            op += match_len;
        }
    }

    if (out_size) *out_size = (uint32_t)(op - dst);
    return 1;
}
