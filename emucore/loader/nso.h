/* nso.h: portable NSO (Nintendo Shared Object) header parser + LZ4.
 *
 * NSO is the executable format inside a program NCA. Its three segments
 * (text, rodata, data) may be LZ4-compressed. This module parses the header
 * and decompresses/relocates segments; getting the bytes to load (the NCA
 * section) requires keys and is handled elsewhere.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NSO_HEADER_SIZE 0x100

typedef struct NsoSegment {
    uint32_t offset;          /* offset of segment data within the NSO      */
    uint32_t location;        /* guest memory location where it maps        */
    uint32_t size;            /* size of the segment once loaded (unpacked) */
    uint32_t compressed_size; /* bytes stored in the file (== size if raw)  */
    uint32_t bss_size;        /* zero-fill after the segment (data segment) */
    int      compressed;      /* LZ4-compressed (flags bit i)               */
} NsoSegment;

typedef struct Nso {
    int      valid;
    uint32_t version;
    uint32_t flags;
    NsoSegment seg[3];        /* text, rodata, data */
    uint8_t  build_id[0x20];
    uint64_t entry;           /* text segment location (module entry) */
} Nso;

typedef enum {
    NSO_OK = 0,
    NSO_ERR_TOO_SMALL,
    NSO_ERR_BAD_MAGIC,
    NSO_ERR_RANGE,
    NSO_ERR_ZBIC,      /* ZBIC-compressed (unsupported) */
} NsoResult;

const char *Nso_ResultName(NsoResult r);

NsoResult Nso_Parse(Nso *n, const void *data, uint64_t size);

/* Standard LZ4 block decompression. Returns 1 on success and sets *out_size;
 * 0 on malformed input or if the result would not fit in dst_cap. */
int Nso_Lz4Decompress(const uint8_t *src, uint32_t src_size,
                      uint8_t *dst, uint32_t dst_cap, uint32_t *out_size);

#ifdef __cplusplus
}
#endif
