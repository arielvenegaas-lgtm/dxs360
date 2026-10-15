/* loader.cpp: see loader.h. Small-endian header decoding with sanity checks. */
#include <string.h>

#include "loader.h"

static uint32_t Rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int Has(const char *p, const char *magic) {
    return p[0] == magic[0] && p[1] == magic[1] &&
           p[2] == magic[2] && p[3] == magic[3];
}

/* A segment is trusted only if it lies wholly within the bytes we actually
 * read. A zero-length segment pointing at the end is allowed. */
static int SegOk(uint32_t off, uint32_t size, uint32_t len) {
    if (size == 0) return off <= len;
    return off <= len && size <= len - off;
}

/* NSO header (little-endian). Offsets are stable across current firmware:
 *   0x00 "NSO0"        0x04 version      0x0C flags
 *   0x10 text  {off,size}  0x18 module off  0x20 rodata {off,size}
 *   0x28 data  {off,size}  0x30 bss size    0x38 build id (32 bytes)
 */
static int ParseNso(const unsigned char *b, uint32_t len, ModuleInfo *m) {
    if (len < 0x58) return 0; /* need through the build id */

    m->version       = Rd32(b + 0x04);
    m->flags         = Rd32(b + 0x0C);
    m->text_off      = Rd32(b + 0x10);
    m->text_size     = Rd32(b + 0x14);
    m->ro_off        = Rd32(b + 0x20);
    m->ro_size       = Rd32(b + 0x24);
    m->data_off      = Rd32(b + 0x28);
    m->data_size     = Rd32(b + 0x2C);
    m->bss_size      = Rd32(b + 0x30);

    m->has_segments  = SegOk(m->text_off, m->text_size, len) &&
                       SegOk(m->ro_off,   m->ro_size,   len) &&
                       SegOk(m->data_off, m->data_size, len);

    memcpy(m->build_id, b + 0x38, 32);
    m->has_build_id = 1; /* a valid NSO header always carries one */
    m->declared_size = 0;
    return 1;
}

/* NRO header. We only trust the four leading words (magic, header size, total
 * size, flags); the asset/segment layout that follows is version-dependent, so
 * no segment sizes are reported for NROs. */
static int ParseNro(const unsigned char *b, uint32_t len, ModuleInfo *m) {
    if (len < 0x10) return 0;
    uint32_t nrosize = Rd32(b + 0x08);
    m->flags         = Rd32(b + 0x0C);
    m->declared_size = nrosize;
    m->version       = Rd32(b + 0x04);
    m->has_segments  = 0;
    m->has_build_id  = 0;
    return 1;
}

int Loader_ProbeModule(const void *data, uint32_t len, ModuleInfo *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!data || !out || len < 4) return 0;

    const unsigned char *b = (const unsigned char *)data;

    if (Has((const char *)b, "NSO0")) {
        out->is_nso = 1;
        if (!ParseNso(b, len, out)) { memset(out, 0, sizeof(*out)); return 0; }
        out->valid = 1;
        return 1;
    }
    if (Has((const char *)b, "NRO0")) {
        out->is_nso = 0;
        if (!ParseNro(b, len, out)) { memset(out, 0, sizeof(*out)); return 0; }
        out->valid = 1;
        return 1;
    }
    return 0;
}
