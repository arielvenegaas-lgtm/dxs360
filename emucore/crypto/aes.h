/* aes.h: portable AES (FIPS-197) with the modes NCA content needs.
 *
 * Plain C99, no external dependencies: the frontend runs bare-metal on the
 * Xenon and the host tests must be reproducible everywhere. Implements:
 *   - AES-128/192/256 block encrypt/decrypt (FIPS-197)
 *   - ECB (used for the NCA key area and the ticket title key)
 *   - CTR  (used for NCA section data; counter updates as NCA specifies)
 *   - XTS  (used for the NCA header; IEEE 1619 core with Nintendo's tweak)
 *
 * Nothing here trusts host byte order: all words are built from bytes.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AesKey {
    uint32_t rk[60]; /* expanded round keys (max 14 rounds + 1) */
    int      nr;     /* number of rounds: 10/12/14               */
} AesKey;

/* key_len must be 16, 24 or 32; returns 1 on success, 0 otherwise. */
int  Aes_Init(AesKey *k, const uint8_t *key, size_t key_len);

void Aes_EncryptBlock(const AesKey *k, const uint8_t in[16], uint8_t out[16]);
void Aes_DecryptBlock(const AesKey *k, const uint8_t in[16], uint8_t out[16]);

/* ECB over a whole buffer. `len` must be a multiple of 16. */
void Aes_EcbEncrypt(const uint8_t *key, size_t key_len, uint8_t *out,
                    const uint8_t *in, size_t len);
void Aes_EcbDecrypt(const uint8_t *key, size_t key_len, uint8_t *out,
                    const uint8_t *in, size_t len);

/* Generic CTR: the 16-byte counter block is incremented as a big-endian
 * integer between blocks. `out` and `in` may alias. */
void Aes_CtrXcrypt(const uint8_t *key, size_t key_len, const uint8_t ctr[16],
                   uint8_t *out, const uint8_t *in, size_t len);

/* XTS core (IEEE 1619) with a caller-supplied 16-byte tweak value. The data
 * key is key[0..15], the tweak key is key[16..31] (key_len must be 32 or 64).
 * `len` must be a multiple of 16. */
void Aes_XtsDecrypt(const uint8_t *key, size_t key_len, const uint8_t tweak[16],
                    uint8_t *out, const uint8_t *in, size_t len);
void Aes_XtsEncrypt(const uint8_t *key, size_t key_len, const uint8_t tweak[16],
                    uint8_t *out, const uint8_t *in, size_t len);

/* Nintendo NCA tweak: the 128-bit tweak holds the 64-bit sector number in
 * *little-endian in bytes 8..15* (bytes 0..7 zero). This is the "reversed
 * endianness" the NCA format uses. */
void Aes_NcaTweak(uint8_t tweak[16], uint64_t sector);

/* Decrypt/encrypt `len` bytes of NCA header data starting at absolute byte
 * `offset` (the sector number is derived as offset/0x200 and advances per
 * 0x200-byte sector). `len` must be a multiple of 0x200. */
void Aes_XtsDecryptNca(const uint8_t *key, uint8_t *out, const uint8_t *in,
                       size_t len, uint64_t offset);
void Aes_XtsEncryptNca(const uint8_t *key, uint8_t *out, const uint8_t *in,
                       size_t len, uint64_t offset);

#ifdef __cplusplus
}
#endif
