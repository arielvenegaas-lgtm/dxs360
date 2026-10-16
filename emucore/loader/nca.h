/* nca.h: portable NCA (Nintendo Content Archive) reader.
 *
 * Covers the parts needed to get an executable out of a program NCA:
 *   - header AES-XTS decryption and field parsing
 *   - key-area AES-ECB decryption
 *   - ticket title-key derivation (titlekek)
 *   - section AES-XTS / AES-CTR decryption
 *   - ExeFS (PFS0) "main" NSO extraction
 *
 * Everything operates on caller-provided buffers: no globals and no I/O, so it
 * runs under the host tests and on the console. All offsets are validated
 * against the buffer length before use.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "keys.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NCA_HEADER_SIZE    0xC00
#define NCA_FS_HEADER_SIZE 0x200
#define NCA_MEDIA_UNIT     0x200
#define NCA_KEY_AREA_SIZE  0x40

/* content_type values */
enum {
    NCA_CONTENT_PROGRAM = 0,
    NCA_CONTENT_META    = 1,
    NCA_CONTENT_CONTROL = 2,
    NCA_CONTENT_MANUAL  = 3,
    NCA_CONTENT_DATA    = 4,
    NCA_CONTENT_PUBLIC  = 5,
};

/* section crypt_type values */
enum {
    NCA_CRYPT_NONE = 1,
    NCA_CRYPT_XTS  = 2,
    NCA_CRYPT_CTR  = 3,
    NCA_CRYPT_BKTR = 4,
};

/* section fs_type values */
enum {
    NCA_FS_PFS0  = 2,
    NCA_FS_ROMFS = 3,
};

typedef struct NcaSection {
    int      present;
    uint8_t  partition_type;
    uint8_t  fs_type;
    uint8_t  crypt_type;
    uint8_t  section_ctr[8];
    uint64_t offset;      /* absolute byte offset within the NCA  */
    uint64_t size;        /* section size in bytes                */
    uint64_t pfs0_offset; /* PFS0 (ExeFS) offset, section-relative */
    uint64_t pfs0_size;
} NcaSection;

typedef struct Nca {
    uint32_t magic;
    uint8_t  distribution;
    uint8_t  content_type;
    uint8_t  kaek_ind;
    uint8_t  crypto_type;     /* header field 0x206 */
    uint8_t  crypto_type2;    /* header field 0x220 */
    unsigned key_generation;  /* max(crypto_type,crypto_type2) - 1 (if nonzero) */
    uint64_t nca_size;
    uint64_t title_id;
    int      has_rights_id;
    uint8_t  rights_id[0x10];
    uint8_t  encrypted_key_area[NCA_KEY_AREA_SIZE];
    NcaSection section[4];
} Nca;

/* FsHeader partition type (field 0x2 of each FsHeader). */
#define NCA_PARTITION_ROMFS 0
#define NCA_PARTITION_PFS0  1

typedef enum {
    NCA_OK = 0,
    NCA_ERR_TOO_SMALL,
    NCA_ERR_BAD_MAGIC,
    NCA_ERR_NO_HEADER_KEY,
    NCA_ERR_RANGE,
    NCA_ERR_UNSUPPORTED,
    NCA_ERR_NO_SECTION,
    NCA_ERR_NO_NSO,
    NCA_ERR_NEED_KEY,
    NCA_ERR_NO_KEYS,
} NcaResult;

const char *Nca_ResultName(NcaResult r);

/* Decrypt and parse the NCA header. `data` must be >= NCA_HEADER_SIZE bytes and
 * point at the start of the NCA. Requires `keys` to hold header_key. */
NcaResult Nca_Parse(Nca *nca, const void *data, uint64_t size, const ProdKeys *keys);

/* AES-ECB decrypt the 0x40-byte key area into `out`. */
NcaResult Nca_DecryptKeyArea(const Nca *nca, const ProdKeys *keys, uint8_t out[NCA_KEY_AREA_SIZE]);

/* Fill `key` (capacity >= 32) and *key_len with the content key for section i.
 * For rights-id content pass the 16-byte `titlekey`; otherwise it is derived
 * from the key area. Returns NCA_OK, or NCA_ERR_NEED_KEY when a rights-id title
 * key is required but NULL was passed. */
NcaResult Nca_SectionKey(const Nca *nca, int i, const ProdKeys *keys,
                         const uint8_t *titlekey, uint8_t *key, size_t *key_len);

/* Decrypt section i. `in` points at section i's first byte (sec->size bytes).
 * `out` receives sec->size bytes. Pass key==NULL/key_len==0 for CRYPT_NONE. */
NcaResult Nca_DecryptSection(const Nca *nca, int i, const uint8_t *in,
                             uint8_t *out, const uint8_t *key, size_t key_len);

/* Ticket helpers. Nca_ParseTicket fills the encrypted title key block (0x10),
 * master key revision and rights id. Nca_TitleKey also decrypts the title key
 * with titlekek[revision]. */
NcaResult Nca_ParseTicket(const void *ticket, uint64_t size,
                          uint8_t enc_titlekey[0x10], uint8_t *master_key_rev,
                          uint8_t rights_id[0x10]);
NcaResult Nca_TitleKey(const void *ticket, uint64_t size, const ProdKeys *keys,
                       uint8_t out_titlekey[0x10], uint8_t out_rights_id[0x10]);

/* Decrypt ExeFS section i of `nca` (whose bytes are `nca_data`) into `out`
 * (>= sec->size bytes) and locate the "main" NSO within it. On success *nso and
 * *nso_len point inside `out`. */
NcaResult Nca_ExtractExefs(const Nca *nca, int i, const uint8_t *nca_data,
                           uint64_t nca_size, const ProdKeys *keys,
                           const uint8_t *titlekey, uint8_t *out, size_t out_cap,
                           const uint8_t **nso, uint64_t *nso_len);

/* Like Nca_ExtractExefs but `sec_data` points directly at section i's first
 * byte (already read into memory), rather than at the start of the NCA. Used
 * by the streaming extractor so it never needs the whole NCA resident. */
NcaResult Nca_DecryptExefsSection(const Nca *nca, int i, const ProdKeys *keys,
                                  const uint8_t *titlekey, const uint8_t *sec_data,
                                  uint8_t *out, size_t out_cap,
                                  const uint8_t **nso, uint64_t *nso_len);

/* Decrypt PFS0 section i into `out` (>= sec->size bytes) without locating any
 * particular member. `sec_data` points at the section's first byte. The caller
 * parses the resulting PFS0 (at sec->pfs0_offset / sec->pfs0_size) to reach the
 * individual module NSOs. */
NcaResult Nca_DecryptExefsRaw(const Nca *nca, int i, const ProdKeys *keys,
                              const uint8_t *titlekey, const uint8_t *sec_data,
                              uint8_t *out, size_t out_cap);

/* Convenience: from a full NSP image, find the program NCA + ticket, decrypt it
 * and locate the ExeFS "main" NSO. `scratch`/`scratch_cap` hold the decrypted
 * section; on success *nso and *nso_len point into `scratch`. */
NcaResult Nsp_ExtractMain(const ProdKeys *keys, const uint8_t *nsp, uint64_t nsp_size,
                          uint8_t *scratch, size_t scratch_cap,
                          const uint8_t **nso, uint64_t *nso_len);

/* Random-access byte reader: read `len` bytes at absolute offset `off` into
 * `buf`. Returns bytes read (>= 0), or -1 on error. */
typedef long (*NcaReadFn)(void *ctx, uint64_t off, void *buf, size_t len);

/* Streaming variant of Nsp_ExtractMain for containers too large to hold in RAM
 * (a retail NSP is ~140 MB). Instead of the whole NSP it reads only the pieces
 * it needs -- the PFS0 header, each candidate NCA header, the matching ticket,
 * and finally the ExeFS section -- through `rd`. `nsp_size` is the real
 * container size; `scratch` (>= the ExeFS section size, or >= 64 KB) holds the
 * decrypted section and the working header read. On success *nso and *nso_len
 * point into `scratch`. Returns the same codes as Nsp_ExtractMain. */
NcaResult Nsp_ExtractMainStream(const ProdKeys *keys, NcaReadFn rd, void *ctx,
                                uint64_t nsp_size, uint8_t *scratch, size_t scratch_cap,
                                const uint8_t **nso, uint64_t *nso_len);

/* Streaming variant that returns the WHOLE decrypted ExeFS PFS0 (all module
 * NSOs, "main.npdm" included) instead of only "main". `scratch` must hold the
 * ExeFS section; on success *exefs and *exefs_len point at the PFS0 image
 * inside `scratch` (offset by the section's pfs0_offset). The caller parses it
 * with Pfs0_Parse and loads every member. */
NcaResult Nsp_ExtractExefsStream(const ProdKeys *keys, NcaReadFn rd, void *ctx,
                                 uint64_t nsp_size, uint8_t *scratch, size_t scratch_cap,
                                 const uint8_t **exefs, uint64_t *exefs_len);

#ifdef __cplusplus
}
#endif
