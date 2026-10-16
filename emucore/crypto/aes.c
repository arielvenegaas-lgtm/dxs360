/* aes.c: portable AES-128/192/256 (FIPS-197), ECB, CTR and XTS.
 *
 * Plain C99, no external dependencies. The S-box tables are derived from the
 * AES field at first use (no hand-typed magic tables), which keeps this file
 * auditable and immune to transcription slips. All arithmetic is byte-oriented
 * so the result is identical on the little-endian host and the big-endian
 * Xenon.
 */
#include "aes.h"

#include <string.h>

static uint8_t sbox[256];
static uint8_t inv_sbox[256];
static int     tables_ready = 0;

/* Multiply in GF(2^8) modulo the AES polynomial x^8 + x^4 + x^3 + x + 1. */
static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    int i;
    for (i = 0; i < 8; ++i) {
        if (b & 1) p ^= a;
        {
            uint8_t hi = (uint8_t)(a & 0x80);
            a = (uint8_t)(a << 1);
            if (hi) a ^= 0x1b;
        }
        b = (uint8_t)(b >> 1);
    }
    return p;
}

static uint8_t rotl8(uint8_t x, int n) {
    return (uint8_t)((x << n) | (x >> (8 - n)));
}

/* Round constant for key expansion step n (1-based): 2^(n-1) in GF(2^8). */
static uint8_t rcon_byte(int n) {
    uint8_t c = 1;
    while (--n > 0) c = gmul(c, 2);
    return c;
}

static uint32_t sub_word(uint32_t t) {
    return ((uint32_t)sbox[(t >> 24) & 0xff] << 24) |
           ((uint32_t)sbox[(t >> 16) & 0xff] << 16) |
           ((uint32_t)sbox[(t >> 8) & 0xff] << 8) |
           (uint32_t)sbox[t & 0xff];
}

static void build_tables(void) {
    int x, y;
    if (tables_ready) return;
    for (x = 0; x < 256; ++x) {
        uint8_t inv = 0;
        if (x != 0) {
            for (y = 1; y < 256; ++y) {
                if (gmul((uint8_t)x, (uint8_t)y) == 1) { inv = (uint8_t)y; break; }
            }
        }
        sbox[x] = (uint8_t)(inv ^ rotl8(inv, 1) ^ rotl8(inv, 2) ^
                            rotl8(inv, 3) ^ rotl8(inv, 4) ^ 0x63);
    }
    for (x = 0; x < 256; ++x) inv_sbox[sbox[x]] = (uint8_t)x;
    tables_ready = 1;
}

/* ---- key expansion ----------------------------------------------------- */

int Aes_Init(AesKey *k, const uint8_t *key, size_t key_len) {
    int nk, nr, i;
    uint32_t *w;

    if (key_len != 16 && key_len != 24 && key_len != 32) return 0;
    build_tables();

    nk = (int)key_len / 4;
    nr = nk + 6;
    k->nr = nr;
    w = k->rk;

    for (i = 0; i < nk; ++i)
        w[i] = ((uint32_t)key[4 * i] << 24) | ((uint32_t)key[4 * i + 1] << 16) |
               ((uint32_t)key[4 * i + 2] << 8) | (uint32_t)key[4 * i + 3];

    for (i = nk; i < 4 * (nr + 1); ++i) {
        uint32_t t = w[i - 1];
        if (i % nk == 0) {
            t = (t << 8) | (t >> 24); /* RotWord */
            t = sub_word(t);
            t ^= (uint32_t)rcon_byte(i / nk) << 24;
        } else if (nk > 6 && i % nk == 4) {
            t = sub_word(t);
        }
        w[i] = w[i - nk] ^ t;
    }
    return 1;
}

/* ---- block cipher ------------------------------------------------------ */

static void add_round_key(uint8_t s[16], const uint32_t *rk) {
    int c, r;
    for (c = 0; c < 4; ++c) {
        uint32_t w = rk[c];
        for (r = 0; r < 4; ++r)
            s[4 * c + r] ^= (uint8_t)((w >> (24 - 8 * r)) & 0xff);
    }
}

void Aes_EncryptBlock(const AesKey *k, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    int i, r, c;

    memcpy(s, in, 16);
    add_round_key(s, k->rk);

    for (r = 1; r < k->nr; ++r) {
        /* SubBytes */
        for (i = 0; i < 16; ++i) s[i] = sbox[s[i]];
        /* ShiftRows: row r shifts left by r (state index = row + 4*col) */
        for (c = 0; c < 4; ++c)
            for (i = 0; i < 4; ++i)
                t[4 * c + i] = s[4 * ((c + i) & 3) + i];
        memcpy(s, t, 16);
        /* MixColumns */
        for (c = 0; c < 4; ++c) {
            uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
            uint8_t b0 = (uint8_t)(gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3);
            uint8_t b1 = (uint8_t)(a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3);
            uint8_t b2 = (uint8_t)(a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3));
            uint8_t b3 = (uint8_t)(gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2));
            s[4 * c + 0] = b0; s[4 * c + 1] = b1; s[4 * c + 2] = b2; s[4 * c + 3] = b3;
        }
        add_round_key(s, k->rk + 4 * r);
    }

    for (i = 0; i < 16; ++i) s[i] = sbox[s[i]];
    for (c = 0; c < 4; ++c)
        for (i = 0; i < 4; ++i) t[4 * c + i] = s[4 * ((c + i) & 3) + i];
    memcpy(s, t, 16);
    add_round_key(s, k->rk + 4 * k->nr);
    memcpy(out, s, 16);
}

static void inv_sub_bytes(uint8_t s[16]) {
    int i;
    for (i = 0; i < 16; ++i) s[i] = inv_sbox[s[i]];
}

static void inv_shift_rows(uint8_t s[16]) {
    uint8_t t[16];
    int i, c;
    for (c = 0; c < 4; ++c)
        for (i = 0; i < 4; ++i) t[4 * c + i] = s[4 * ((c - i + 4) & 3) + i];
    memcpy(s, t, 16);
}

static void inv_mix_columns(uint8_t s[16]) {
    int c;
    for (c = 0; c < 4; ++c) {
        uint8_t a0 = s[4 * c + 0], a1 = s[4 * c + 1], a2 = s[4 * c + 2], a3 = s[4 * c + 3];
        s[4 * c + 0] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
        s[4 * c + 1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
        s[4 * c + 2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
        s[4 * c + 3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
    }
}

void Aes_DecryptBlock(const AesKey *k, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    int r;

    memcpy(s, in, 16);
    add_round_key(s, k->rk + 4 * k->nr);
    inv_shift_rows(s);
    inv_sub_bytes(s);

    /* Strict reverse of encryption: each round undoes (SB, SR, MC, ARK_r) as
     * (ARK_r, InvMC, InvSR, InvSB). */
    for (r = k->nr - 1; r >= 1; --r) {
        add_round_key(s, k->rk + 4 * r);
        inv_mix_columns(s);
        inv_shift_rows(s);
        inv_sub_bytes(s);
    }

    add_round_key(s, k->rk);
    memcpy(out, s, 16);
}

/* ---- ECB / CTR / XTS --------------------------------------------------- */

void Aes_EcbEncrypt(const uint8_t *key, size_t key_len, uint8_t *out,
                    const uint8_t *in, size_t len) {
    AesKey k; size_t i;
    Aes_Init(&k, key, key_len);
    for (i = 0; i + 16 <= len; i += 16) Aes_EncryptBlock(&k, in + i, out + i);
}

void Aes_EcbDecrypt(const uint8_t *key, size_t key_len, uint8_t *out,
                    const uint8_t *in, size_t len) {
    AesKey k; size_t i;
    Aes_Init(&k, key, key_len);
    for (i = 0; i + 16 <= len; i += 16) Aes_DecryptBlock(&k, in + i, out + i);
}

void Aes_CtrXcrypt(const uint8_t *key, size_t key_len, const uint8_t ctr[16],
                   uint8_t *out, const uint8_t *in, size_t len) {
    AesKey k; uint8_t counter[16], ks[16]; size_t pos;
    Aes_Init(&k, key, key_len);
    memcpy(counter, ctr, 16);
    for (pos = 0; pos < len; pos += 16) {
        size_t n = len - pos; int i;
        if (n > 16) n = 16;
        Aes_EncryptBlock(&k, counter, ks);
        for (i = 0; i < (int)n; ++i) out[pos + i] = (uint8_t)(in[pos + i] ^ ks[i]);
        for (i = 15; i >= 0; --i) { if (++counter[i] != 0) break; }
    }
}

static void xts_mul_alpha(uint8_t t[16]) {
    uint8_t carry = (uint8_t)(t[15] >> 7);
    int i;
    for (i = 15; i > 0; --i) t[i] = (uint8_t)((t[i] << 1) | (t[i - 1] >> 7));
    t[0] = (uint8_t)(t[0] << 1);
    if (carry) t[0] ^= 0x87;
}

static void xts_run(const uint8_t *key, size_t key_len, const uint8_t tweak[16],
                    uint8_t *out, const uint8_t *in, size_t len, int decrypt) {
    AesKey k1, k2;
    uint8_t T[16], x[16];
    size_t half = key_len / 2, pos;
    int i;

    Aes_Init(&k1, key, half);
    Aes_Init(&k2, key + half, half);
    Aes_EncryptBlock(&k2, tweak, T);

    for (pos = 0; pos + 16 <= len; pos += 16) {
        for (i = 0; i < 16; ++i) x[i] = (uint8_t)(in[pos + i] ^ T[i]);
        if (decrypt) Aes_DecryptBlock(&k1, x, x);
        else         Aes_EncryptBlock(&k1, x, x);
        for (i = 0; i < 16; ++i) out[pos + i] = (uint8_t)(x[i] ^ T[i]);
        xts_mul_alpha(T);
    }
}

void Aes_XtsDecrypt(const uint8_t *key, size_t key_len, const uint8_t tweak[16],
                    uint8_t *out, const uint8_t *in, size_t len) {
    xts_run(key, key_len, tweak, out, in, len, 1);
}

void Aes_XtsEncrypt(const uint8_t *key, size_t key_len, const uint8_t tweak[16],
                    uint8_t *out, const uint8_t *in, size_t len) {
    xts_run(key, key_len, tweak, out, in, len, 0);
}

void Aes_NcaTweak(uint8_t tweak[16], uint64_t sector) {
    int j;
    memset(tweak, 0, 16);
    for (j = 0; j < 8; ++j) tweak[15 - j] = (uint8_t)((sector >> (8 * j)) & 0xff);
}

void Aes_XtsDecryptNca(const uint8_t *key, uint8_t *out, const uint8_t *in,
                       size_t len, uint64_t offset) {
    size_t pos;
    for (pos = 0; pos + 0x200 <= len; pos += 0x200) {
        uint8_t tw[16];
        Aes_NcaTweak(tw, (offset + pos) / 0x200);
        Aes_XtsDecrypt(key, 32, tw, out + pos, in + pos, 0x200);
    }
}

void Aes_XtsEncryptNca(const uint8_t *key, uint8_t *out, const uint8_t *in,
                       size_t len, uint64_t offset) {
    size_t pos;
    for (pos = 0; pos + 0x200 <= len; pos += 0x200) {
        uint8_t tw[16];
        Aes_NcaTweak(tw, (offset + pos) / 0x200);
        Aes_XtsEncrypt(key, 32, tw, out + pos, in + pos, 0x200);
    }
}
