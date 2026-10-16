/* keys.h: portable parser for the common key lists the NCA content layer needs.
 *
 * Eden360 needs three kinds of key from a user-supplied `prod.keys`:
 *   - header_key                     (AES-XTS key for the NCA header)
 *   - titlekek_XX                    (per master-key-revision title key KEK)
 *   - key_area_key_<type>_XX         (application/ocean/system KEK)
 *
 * Only the keys actually used are extracted; every other line (sources, other
 * key types, comments) is ignored. No global state and no I/O: the caller
 * reads the file (however it likes) and hands the parser a buffer, so this is
 * testable on the host and usable bare-metal.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KEYS_MAX_REVISION 0x20 /* master key revisions 0x00..0x1F */
#define KEYS_MAX_VALUE    0x20 /* largest single key we store (bytes) */

typedef enum {
    KEYS_KAEK_APPLICATION = 0,
    KEYS_KAEK_OCEAN       = 1,
    KEYS_KAEK_SYSTEM      = 2,
    KEYS_KAEK_COUNT       = 3,
} KeysKaekType;

typedef struct ProdKeys {
    int      have_header_key;
    uint32_t header_key_len;                     /* 16 or 32 */
    uint8_t  header_key[KEYS_MAX_VALUE];

    int      have_titlekek[KEYS_MAX_REVISION];
    uint8_t  titlekek[KEYS_MAX_REVISION][16];

    int      have_kaek[KEYS_KAEK_COUNT][KEYS_MAX_REVISION];
    uint8_t  kaek[KEYS_KAEK_COUNT][KEYS_MAX_REVISION][16];

    uint32_t count; /* number of recognised keys parsed */
} ProdKeys;

typedef enum {
    KEYS_OK = 0,
    KEYS_ERR_IO,   /* reserved */
    KEYS_ERR_NONE, /* nothing usable was found */
} KeysResult;

const char *Keys_ResultName(KeysResult r);

void Keys_Init(ProdKeys *k);

/* Parse a `prod.keys` text buffer of `len` bytes. Returns KEYS_OK when at
 * least one recognised key was parsed, KEYS_ERR_NONE otherwise. */
KeysResult Keys_Parse(ProdKeys *k, const void *data, size_t len);

/* Lookups. All return NULL when the key is absent. */
const uint8_t *Keys_HeaderKey(const ProdKeys *k, uint32_t *out_len);
const uint8_t *Keys_Titlekek(const ProdKeys *k, unsigned revision);
const uint8_t *Keys_Kaek(const ProdKeys *k, int kaek_type, unsigned revision);

#ifdef __cplusplus
}
#endif
