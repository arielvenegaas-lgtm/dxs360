/* pfs0.h: portable, bounds-checked PFS0 parser (NSP outer container).
 *
 * PFS0 is the unencrypted partition filesystem used as the outermost container
 * of an NSP (and of the ExeFS-equivalent packages). No keys are needed to read
 * it. The parser never trusts sizes in the file: every entry range and every
 * name is validated against the buffer before being exposed.
 *
 * Note: the canonical PFS0 stores entry offsets relative to the start of the
 * file. Some repacking tools instead store them relative to the end of the
 * header; the parser detects which layout a given file uses and exposes it via
 * `relative`, so callers get correct absolute offsets either way.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PFS0_MAX_FILES 256

typedef struct Pfs0Entry {
    uint64_t offset;   /* absolute offset of the file data in the buffer */
    uint64_t size;     /* file size in bytes                             */
    uint32_t name_off; /* name offset within the string table            */
} Pfs0Entry;

typedef struct Pfs0 {
    const uint8_t *data;
    uint64_t size;
    uint32_t count;
    uint32_t strtab_size;
    uint64_t header_size; /* 0x10 + count*0x18 + strtab_size */
    int      relative;    /* 1 if entry offsets are header-relative */
    Pfs0Entry entries[PFS0_MAX_FILES];
} Pfs0;

typedef enum {
    PFS0_OK = 0,
    PFS0_ERR_TOO_SMALL,
    PFS0_ERR_BAD_MAGIC,
    PFS0_ERR_TOO_MANY,
    PFS0_ERR_RANGE,
} Pfs0Result;

const char *Pfs0_ResultName(Pfs0Result r);

Pfs0Result Pfs0_Parse(Pfs0 *p, const void *data, uint64_t size);

/* Like Pfs0_Parse, but only `hdr_len` bytes of the header/string table are
 * guaranteed present while the container's real size is `file_size`. Entry
 * data is validated against `file_size` (so a huge NSP can be probed from a
 * small header read), while the header/string table are read only within
 * `hdr_len`. Entry data must not be dereferenced until fully read. */
Pfs0Result Pfs0_ParseEx(Pfs0 *p, const void *hdr, uint64_t hdr_len, uint64_t file_size);

/* NUL-terminated name for entry i, or "" if i is out of range. */
const char *Pfs0_Name(const Pfs0 *p, uint32_t i);

/* Data pointer for entry i; *out_size receives the size. NULL if invalid. */
const uint8_t *Pfs0_Data(const Pfs0 *p, uint32_t i, uint64_t *out_size);

/* Index of the first entry whose name equals `name`, or -1. */
int Pfs0_Find(const Pfs0 *p, const char *name);

#ifdef __cplusplus
}
#endif
