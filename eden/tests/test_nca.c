/* test_nca.c: host tests for the NCA content layer.
 *
 * Two kinds of check:
 *   1. Known-answer tests for the crypto primitives (FIPS-197, SP800-38A CTR,
 *      IEEE 1619 XTS) so a regression is caught before the layout logic.
 *   2. Synthetic end-to-end round trips: build an NSP -> NCA (XTS header, ECB
 *      key area, XTS/CTR section, optional rights-id ticket) -> ExeFS PFS0 with
 *      a "main" NSO, then run Nsp_ExtractMain and verify the exact NSO bytes
 *      come back out and parse as a real NSO. Covered variants:
 *        - non-rights, section AES-XTS (key area)
 *      - non-rights, section AES-CTR (key area)
 *      - rights-id, AES-CTR with a ticket title key
 *   3. On-disk layout locks (independent of the synthetic builder): the FsEntry
 *      table stride (4 x 0x10 at 0x240) and the ticket signature-data size Y
 *      for RSA-4096/2048, ECDSA (full 0x40 pad) and big-endian type encodings,
 *      including a trimmed (0x180-byte) ticket_data as shipped in real NSPs.
 *
 * Build (from the repo root):
 *   gcc -std=c99 -Wall -Wextra -I emucore/crypto -I emucore/loader \
 *       eden/tests/test_nca.c emucore/crypto/aes.c emucore/loader/keys.c \
 *       emucore/loader/nca.c emucore/loader/pfs0.c emucore/loader/nso.c \
 *       -o /tmp/test_nca && /tmp/test_nca
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aes.h"
#include "keys.h"
#include "nca.h"
#include "pfs0.h"
#include "nso.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++fails; } } while (0)

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

static void fromhex(uint8_t *b, const char *h) {
    int i;
    for (i = 0; i < (int)strlen(h) / 2; i++) {
        unsigned v; sscanf(h + 2 * i, "%2x", &v); b[i] = (uint8_t)v;
    }
}
static int hexeq(const uint8_t *b, const char *h) {
    int i;
    for (i = 0; i < (int)strlen(h) / 2; i++) {
        unsigned v; sscanf(h + 2 * i, "%2x", &v); if (b[i] != (uint8_t)v) return 0;
    }
    return 1;
}

/* ---------------- crypto KATs ---------------- */

static void test_crypto(void) {
    uint8_t pt[16]; fromhex(pt, "00112233445566778899aabbccddeeff");

    {
        uint8_t k[16], c[16], d[16]; AesKey a;
        fromhex(k, "000102030405060708090a0b0c0d0e0f");
        Aes_Init(&a, k, 16); Aes_EncryptBlock(&a, pt, c);
        CHECK(hexeq(c, "69c4e0d86a7b0430d8cdb78070b4c55a"), "AES-128 enc KAT");
        Aes_DecryptBlock(&a, c, d); CHECK(memcmp(d, pt, 16) == 0, "AES-128 dec");
    }
    {
        uint8_t k[24], c[16]; AesKey a;
        fromhex(k, "000102030405060708090a0b0c0d0e0f1011121314151617");
        Aes_Init(&a, k, 24); Aes_EncryptBlock(&a, pt, c);
        CHECK(hexeq(c, "dda97ca4864cdfe06eaf70a0ec0d7191"), "AES-192 enc KAT");
    }
    {
        uint8_t k[32], c[16]; AesKey a;
        fromhex(k, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
        Aes_Init(&a, k, 32); Aes_EncryptBlock(&a, pt, c);
        CHECK(hexeq(c, "8ea2b7ca516745bfeafc49904b496089"), "AES-256 enc KAT");
    }
    {
        uint8_t k[16], ctr[16], in[64], out[64];
        fromhex(k, "2b7e151628aed2a6abf7158809cf4f3c");
        fromhex(ctr, "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
        fromhex(in, "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                    "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
        Aes_CtrXcrypt(k, 16, ctr, out, in, 64);
        CHECK(hexeq(out, "874d6191b620e3261bef6864990db6ce"
                         "9806f66b7970fdff8617187bb9fffdff"
                         "5ae4df3edbd5d35e5b4f09020db03eab"
                         "1e031dda2fbe03d1792170a0f3009cee"), "AES-128-CTR KAT");
    }
    {
        uint8_t k[32], tw[16], p32[32], ct[32];
        fromhex(k, "2718281828459045235360287471352631415926535897932384626433832795");
        fromhex(tw, "00000000000000000000000000000000");
        fromhex(p32, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
        Aes_XtsEncrypt(k, 32, tw, ct, p32, 32);
        CHECK(hexeq(ct, "27a7479befa1d476489f308cd4cfa6e2a96e4bbe3208ff25287dd3819616e89c"), "AES-XTS IEEE1619 KAT");
    }
}

/* ---------------- keys parser ---------------- */

static void test_keys(void) {
    static const char text[] =
        "# prod.keys sample\n"
        "header_key = 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\n"
        "titlekek_00 = 202122232425262728292a2b2c2d2e2f\n"
        "key_area_key_application_00 = 303132333435363738393a3b3c3d3e3f\n"
        "some_source = deadbeef\n"
        "  \n"
        "key_area_key_ocean_1f = ffffffffffffffffffffffffffffffff\n";
    ProdKeys k;
    Keys_Init(&k);
    CHECK(Keys_Parse(&k, text, sizeof(text) - 1) == KEYS_OK, "keys parse");
    CHECK(k.count == 4, "keys count");
    CHECK(k.have_header_key && k.header_key_len == 32, "keys header_key");
    CHECK(Keys_Titlekek(&k, 0) && hexeq(Keys_Titlekek(&k, 0), "202122232425262728292a2b2c2d2e2f"), "keys titlekek_00");
    CHECK(Keys_Titlekek(&k, 1) == NULL, "keys titlekek_01 absent");
    CHECK(Keys_Kaek(&k, KEYS_KAEK_APPLICATION, 0) != NULL, "keys kaek app 00");
    CHECK(Keys_Kaek(&k, KEYS_KAEK_OCEAN, 0x1f) != NULL, "keys kaek ocean 1f");
    CHECK(Keys_Kaek(&k, KEYS_KAEK_SYSTEM, 0) == NULL, "keys kaek system absent");
}

/* ---------------- synthetic image builders ---------------- */

/* Minimal but structurally valid NSO: text (2 insns), rodata (raw), data (raw). */
static uint64_t build_nso(uint8_t *buf) {
    uint32_t text_off = 0x100, ro_off = 0x108, data_off = 0x110;
    memset(buf, 0, data_off + 8);
    memcpy(buf, "NSO0", 4);
    put32(buf + 0x0C, 0);
    put32(buf + 0x10, text_off); put32(buf + 0x14, 0x10000000u); put32(buf + 0x18, 8); put32(buf + 0x1C, 0);
    put32(buf + 0x20, ro_off);   put32(buf + 0x24, 0x10001000u); put32(buf + 0x28, 8); put32(buf + 0x2C, 0);
    put32(buf + 0x30, data_off); put32(buf + 0x34, 0x10002000u); put32(buf + 0x38, 8); put32(buf + 0x3C, 0);
    put32(buf + 0x60, 8); put32(buf + 0x64, 8); put32(buf + 0x68, 8);
    put32(buf + text_off + 0, 0xD2824681u); /* movz x1,#0x1234 */
    put32(buf + text_off + 4, 0xD4200000u); /* brk #0 */
    memcpy(buf + ro_off, "RODATA!!", 8);
    memcpy(buf + data_off, "DATADATA", 8);
    return data_off + 8;
}

/* PFS0 with N entries; entry offsets are header-relative (canonical). */
static uint64_t build_pfs0(uint8_t *buf, const char *const *names, const uint8_t *const *datas,
                           const uint64_t *sizes, int n) {
    uint32_t strtab = 0, i, nst = 0;
    uint64_t header, data_ofs = 0;
    for (i = 0; i < (uint32_t)n; i++) strtab += (uint32_t)strlen(names[i]) + 1;
    header = 0x10 + (uint64_t)n * 0x18 + strtab;

    memset(buf, 0, (size_t)(header + 1));
    memcpy(buf, "PFS0", 4);
    put32(buf + 4, (uint32_t)n);
    put32(buf + 8, strtab);
    put32(buf + 12, 0);

    for (i = 0; i < (uint32_t)n; i++) {           /* file entries */
        uint8_t *e = buf + 0x10 + (uint64_t)i * 0x18;
        put64(e + 0, data_ofs);
        put64(e + 8, sizes[i]);
        put32(e + 16, nst);
        data_ofs += sizes[i];
        nst += (uint32_t)strlen(names[i]) + 1;
    }
    nst = 0;
    for (i = 0; i < (uint32_t)n; i++) {           /* string table */
        memcpy(buf + 0x10 + (uint64_t)n * 0x18 + nst, names[i], strlen(names[i]) + 1);
        nst += (uint32_t)strlen(names[i]) + 1;
    }
    data_ofs = 0;
    for (i = 0; i < (uint32_t)n; i++) {           /* file data */
        memcpy(buf + header + data_ofs, datas[i], (size_t)sizes[i]);
        data_ofs += sizes[i];
    }
    return header + data_ofs;
}

#define SECTION_MEDIA_START 0x10u
#define SECTION_ABS_OFF     0x2000u
#define HASH_LEN            0x200
#define PFS0_OFF            0x200u

/* Build an NSP containing a program NCA (ExeFS "main" = `nso`) plus, when
 * `use_rights`, a matching common ticket. Returns NSP size or 0 on error. */
static uint64_t build_nsp(uint8_t *out, uint64_t out_cap, int use_rights,
                          uint8_t crypt_type, const uint8_t *nso, uint64_t nso_len) {
    static uint8_t exefs[0x1000];
    static uint8_t section[0x8000];
    static uint8_t enc_sec[0x8000];
    static uint8_t rawhdr[NCA_HEADER_SIZE];
    static uint8_t enc_hdr[NCA_HEADER_SIZE];
    static uint8_t nca[0x20000];
    static uint8_t ticket[0x400];
    static uint8_t npdm[0x40];
    uint8_t section_ctr[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    uint8_t rights[16];
    uint8_t hk[32], titlekek[16], kaek[16], dec[0x40], titlekey[16];
    const char *exe_names[2] = { "main", "main.npdm" };
    const uint8_t *exe_datas[2];
    uint64_t exe_sizes[2];
    uint64_t exefs_size, sec_size, nca_size;
    int i;

    fromhex(hk, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    fromhex(titlekek, "202122232425262728292a2b2c2d2e2f");
    fromhex(kaek, "303132333435363738393a3b3c3d3e3f");
    for (i = 0; i < 0x40; i++) dec[i] = (uint8_t)(0x40 + i);
    fromhex(titlekey, "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf");
    for (i = 0; i < 16; i++) rights[i] = (uint8_t)(0xC0 + i);
    memset(npdm, 0, sizeof(npdm)); memcpy(npdm, "META", 4);

    exe_datas[0] = nso;    exe_sizes[0] = nso_len;
    exe_datas[1] = npdm;   exe_sizes[1] = sizeof(npdm);
    exefs_size = build_pfs0(exefs, exe_names, exe_datas, exe_sizes, 2);

    sec_size = (HASH_LEN + exefs_size + 0x1FF) & ~(uint64_t)0x1FF;
    memset(section, 0, sizeof(section));
    memcpy(section + HASH_LEN, exefs, (size_t)exefs_size);

    /* Encrypt the section. */
    if (crypt_type == NCA_CRYPT_XTS) {
        Aes_XtsEncryptNca(dec, enc_sec, section, (size_t)sec_size, 0);
    } else {
        uint8_t ctr[16]; uint64_t ofs = (uint64_t)SECTION_MEDIA_START * NCA_MEDIA_UNIT >> 4;
        const uint8_t *key = use_rights ? titlekey : (dec + 0x20);
        memset(ctr, 0, sizeof(ctr));
        for (i = 0; i < 8; i++) ctr[i] = section_ctr[7 - i];
        for (i = 0; i < 8; i++) { ctr[15 - i] = (uint8_t)(ofs & 0xFF); ofs >>= 8; }
        Aes_CtrXcrypt(key, 16, ctr, enc_sec, section, (size_t)sec_size);
    }

    nca_size = SECTION_MEDIA_START * NCA_MEDIA_UNIT + sec_size;

    /* Header. */
    memset(rawhdr, 0, sizeof(rawhdr));
    memcpy(rawhdr + 0x200, "NCA3", 4);
    rawhdr[0x204] = 0;               /* distribution: download */
    rawhdr[0x205] = NCA_CONTENT_PROGRAM;
    rawhdr[0x206] = 0;               /* crypto_type  */
    rawhdr[0x207] = 0;               /* kaek_ind     */
    put64(rawhdr + 0x208, nca_size);
    put64(rawhdr + 0x210, 0x0100B7D0022EE000ULL);
    rawhdr[0x220] = 0;
    if (use_rights) memcpy(rawhdr + 0x230, rights, 16);

    put32(rawhdr + 0x240, SECTION_MEDIA_START);
    put32(rawhdr + 0x244, SECTION_MEDIA_START + (uint32_t)(sec_size / NCA_MEDIA_UNIT));

    /* encrypted key area */
    Aes_EcbEncrypt(kaek, 16, rawhdr + 0x300, dec, 0x40);

    /* FS header 0 at 0x400 */
    rawhdr[0x400 + 0x2] = 1;            /* PARTITION_PFS0 */
    rawhdr[0x400 + 0x3] = 2;            /* FS_TYPE_PFS0    */
    rawhdr[0x400 + 0x4] = crypt_type;
    memcpy(rawhdr + 0x400 + 0x140, section_ctr, 8);
    put32(rawhdr + 0x400 + 0x8 + 0x20, 0x200);          /* block_size */
    put64(rawhdr + 0x400 + 0x8 + 0x28, 0);              /* hash_table_offset */
    put64(rawhdr + 0x400 + 0x8 + 0x30, HASH_LEN);       /* hash_table_size */
    put64(rawhdr + 0x400 + 0x8 + 0x38, HASH_LEN);       /* pfs0_offset */
    put64(rawhdr + 0x400 + 0x8 + 0x40, exefs_size);     /* pfs0_size */

    Aes_XtsEncryptNca(hk, enc_hdr, rawhdr, NCA_HEADER_SIZE, 0);

    nca_size = SECTION_MEDIA_START * NCA_MEDIA_UNIT + sec_size;
    memset(nca, 0, sizeof(nca));
    memcpy(nca, enc_hdr, NCA_HEADER_SIZE);
    memcpy(nca + (uint64_t)SECTION_MEDIA_START * NCA_MEDIA_UNIT, enc_sec, (size_t)sec_size);

    /* Build the ticket (RSA-2048 common ticket: 0x140 + 0x2C0 = 0x400). */
    memset(ticket, 0, sizeof(ticket));
    if (use_rights) {
        uint64_t y = 0x140;
        uint8_t enc_tk[16];
        put32(ticket, 0x010004);            /* RSA-2048 PKCS#1v1.5 SHA-256 */
        ticket[y + 0x141] = 0;              /* title key type: common */
        ticket[y + 0x145] = 0;              /* master key revision    */
        memcpy(ticket + y + 0x160, rights, 16);
        Aes_EcbEncrypt(titlekek, 16, enc_tk, titlekey, 0x10);
        memcpy(ticket + y + 0x40, enc_tk, 16);
    }

    /* Final NSP container (PFS0). */
    {
        const char *names[2] = { "program.nca", "program.tik" };
        const uint8_t *datas[2] = { nca, ticket };
        uint64_t sizes[2] = { nca_size, sizeof(ticket) };
        int count = use_rights ? 2 : 1;
        if (SECTION_MEDIA_START * NCA_MEDIA_UNIT + sec_size + sizeof(ticket) + 0x100 > out_cap) return 0;
        return build_pfs0(out, names, datas, sizes, count);
    }
}

static void run_case(const char *label, int use_rights, uint8_t crypt_type) {
    static uint8_t nso[0x200];
    static uint8_t nsp[0x40000];
    static uint8_t scratch[0x8000];
    ProdKeys k;
    uint64_t nso_len, nsp_size, out_len = 0;
    const uint8_t *out = NULL;
    NcaResult r;
    Nso parsed;

    Keys_Init(&k);
    {
        static const char keys[] =
            "header_key = 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\n"
            "titlekek_00 = 202122232425262728292a2b2c2d2e2f\n"
            "key_area_key_application_00 = 303132333435363738393a3b3c3d3e3f\n";
        CHECK(Keys_Parse(&k, keys, sizeof(keys) - 1) == KEYS_OK, "case keys parse");
    }

    nso_len = build_nso(nso);
    nsp_size = build_nsp(nsp, sizeof(nsp), use_rights, crypt_type, nso, nso_len);
    CHECK(nsp_size > 0, label);

    r = Nsp_ExtractMain(&k, nsp, nsp_size, scratch, sizeof(scratch), &out, &out_len);
    CHECK(r == NCA_OK, label);
    if (r == NCA_OK) {
        CHECK(out_len == nso_len, "extracted NSO length");
        CHECK(out && memcmp(out, nso, (size_t)nso_len) == 0, "extracted NSO bytes match");
        CHECK(Nso_Parse(&parsed, out, out_len) == NSO_OK, "extracted bytes parse as NSO");
        CHECK(parsed.entry == 0x10000000u, "extracted NSO entry");
    }
}

static void test_synthetic(void) {
    run_case("non-rights XTS", 0, NCA_CRYPT_XTS);
    run_case("non-rights CTR", 0, NCA_CRYPT_CTR);
    run_case("rights-id CTR",   1, NCA_CRYPT_CTR);
}

/* Lock the on-disk header layout (independent of the synthetic builder): the
 * FsEntry table is 4 x 0x10 bytes at 0x240. A stride bug would misread every
 * section after the first. */
static void test_header_layout(void) {
    static uint8_t hdr[NCA_HEADER_SIZE], enc[NCA_HEADER_SIZE];
    static const char keys[] =
        "header_key = 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\n";
    ProdKeys k;
    uint8_t hk[32];
    Nca nca;
    int i;

    Keys_Init(&k);
    CHECK(Keys_Parse(&k, keys, sizeof(keys) - 1) == KEYS_OK, "layout keys");
    fromhex(hk, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");

    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr + 0x200, "NCA3", 4);
    hdr[0x205] = NCA_CONTENT_PROGRAM;
    put64(hdr + 0x208, sizeof(hdr));
    put64(hdr + 0x210, 0x0100B7D0022EE000ULL);
    for (i = 0; i < 4; i++) {
        uint8_t *se = hdr + 0x240 + (uint64_t)i * 0x10;
        uint8_t *fh = hdr + 0x400 + (uint64_t)i * NCA_FS_HEADER_SIZE;
        put32(se, 0x10 + (uint32_t)i * 0x10);            /* media_start (0x200 units) */
        put32(se + 4, 0x18 + (uint32_t)i * 0x10);        /* media_end                 */
        fh[0x2] = (i == 0) ? NCA_PARTITION_PFS0 : NCA_PARTITION_ROMFS;
        fh[0x3] = (i == 0) ? NCA_FS_PFS0 : NCA_FS_ROMFS;
        fh[0x4] = (i == 0) ? NCA_CRYPT_XTS : NCA_CRYPT_CTR;
        fh[0x140] = (uint8_t)(0xA0 + i);
    }
    Aes_XtsEncryptNca(hk, enc, hdr, NCA_HEADER_SIZE, 0);

    CHECK(Nca_Parse(&nca, enc, sizeof(enc), &k) == NCA_OK, "layout parse");
    for (i = 0; i < 4; i++) {
        uint64_t expect = (uint64_t)(0x10 + i * 0x10) * NCA_MEDIA_UNIT;
        CHECK(nca.section[i].present, "layout section present");
        CHECK(nca.section[i].offset == expect, "layout section offset");
        CHECK(nca.section[i].size == 0x8 * NCA_MEDIA_UNIT, "layout section size");
        CHECK(nca.section[i].section_ctr[0] == (uint8_t)(0xA0 + i), "layout section ctr");
    }
    CHECK(nca.section[0].partition_type == NCA_PARTITION_PFS0 && nca.section[0].fs_type == NCA_FS_PFS0,
          "layout sec0 type");
    CHECK(nca.section[0].crypt_type == NCA_CRYPT_XTS, "layout sec0 crypt");
    CHECK(nca.section[1].partition_type == NCA_PARTITION_ROMFS && nca.section[1].fs_type == NCA_FS_ROMFS,
          "layout sec1 type");
    CHECK(nca.section[1].crypt_type == NCA_CRYPT_CTR, "layout sec1 crypt");
}

/* Lock the ticket signature-data size Y for each signature type, including the
 * ECDSA full-pad case (Y = 0x80, not a plain align of 0x40) and a defensive
 * big-endian type encoding. */
static void test_ticket_layout(void) {
    static uint8_t tik[0x600];
    uint8_t rid[0x10];
    int i;

    memset(tik, 0, sizeof(tik));
    put32(tik, 0x010004);                                /* RSA-2048 LE: Y = 0x140 */
    for (i = 0; i < 16; i++) tik[0x140 + 0x160 + i] = (uint8_t)(0x10 + i);
    CHECK(Nca_ParseTicket(tik, sizeof(tik), NULL, NULL, rid) == NCA_OK, "ticket RSA2048 parse");
    CHECK(rid[0] == 0x10 && rid[15] == 0x1F, "ticket RSA2048 rights id");

    /* Real NSPs trim ticket_data to 0x180 bytes: only the trailing "unknown"
     * region is dropped, so every field the loader reads is still present. A
     * ticket of exactly Y + 0x180 = 0x2C0 bytes must parse, while one byte short
     * of the minimum must still be rejected (keeps the check non-vacuous). */
    memset(tik, 0, sizeof(tik));
    put32(tik, 0x010004);                                /* RSA-2048 LE: Y = 0x140 */
    tik[0x140 + 0x141] = 0;                              /* common title key */
    for (i = 0; i < 16; i++) tik[0x140 + 0x160 + i] = (uint8_t)(0x30 + i);
    CHECK(Nca_ParseTicket(tik, 0x140 + 0x180, NULL, NULL, rid) == NCA_OK, "ticket trimmed data parse");
    CHECK(rid[0] == 0x30 && rid[15] == 0x3F, "ticket trimmed rights id");
    CHECK(Nca_ParseTicket(tik, 0x140 + 0x173, NULL, NULL, rid) == NCA_ERR_TOO_SMALL, "ticket short data rejected");

    memset(tik, 0, sizeof(tik));
    put32(tik, 0x010002);                                /* ECDSA: Y = 0x80 */
    for (i = 0; i < 16; i++) tik[0x80 + 0x160 + i] = (uint8_t)(0x20 + i);
    CHECK(Nca_ParseTicket(tik, sizeof(tik), NULL, NULL, rid) == NCA_OK, "ticket ECDSA parse");
    CHECK(rid[0] == 0x20 && rid[15] == 0x2F, "ticket ECDSA rights id");

    memset(tik, 0, sizeof(tik));
    tik[0] = 0x00; tik[1] = 0x01; tik[2] = 0x00; tik[3] = 0x00; /* BE RSA-4096: Y = 0x240 */
    for (i = 0; i < 16; i++) tik[0x240 + 0x160 + i] = (uint8_t)(0x40 + i);
    CHECK(Nca_ParseTicket(tik, sizeof(tik), NULL, NULL, rid) == NCA_OK, "ticket RSA4096 BE parse");
    CHECK(rid[0] == 0x40 && rid[15] == 0x4F, "ticket RSA4096 BE rights id");

    memset(tik, 0, sizeof(tik));
    put32(tik, 0x00ABCDEF);                              /* unknown type */
    CHECK(Nca_ParseTicket(tik, sizeof(tik), NULL, NULL, rid) == NCA_ERR_UNSUPPORTED, "ticket bad type");
}

static void test_negative(void) {
    static uint8_t nso[0x200], nsp[0x40000], scratch[0x8000];
    static const char good_keys[] =
        "header_key = 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\n"
        "titlekek_00 = 202122232425262728292a2b2c2d2e2f\n"
        "key_area_key_application_00 = 303132333435363738393a3b3c3d3e3f\n";
    ProdKeys k, bad;
    uint64_t nso_len, nsp_size, out_len = 0;
    const uint8_t *out = NULL;
    Pfs0 pfs;

    Keys_Init(&k);
    CHECK(Keys_Parse(&k, good_keys, sizeof(good_keys) - 1) == KEYS_OK, "neg good keys");
    nso_len = build_nso(nso);
    nsp_size = build_nsp(nsp, sizeof(nsp), 1, NCA_CRYPT_CTR, nso, nso_len);
    CHECK(nsp_size > 0, "neg nsp built");

    /* A wrong header key must not yield a program NCA. */
    Keys_Init(&bad);
    {
        static const char keys[] =
            "header_key = ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\n"
            "titlekek_00 = 202122232425262728292a2b2c2d2e2f\n"
            "key_area_key_application_00 = 303132333435363738393a3b3c3d3e3f\n";
        CHECK(Keys_Parse(&bad, keys, sizeof(keys) - 1) == KEYS_OK, "neg bad keys parse");
    }
    CHECK(Nsp_ExtractMain(&bad, nsp, nsp_size, scratch, sizeof(scratch), &out, &out_len) != NCA_OK,
          "neg wrong header key rejected");

    /* Missing header key must be reported explicitly. */
    Keys_Init(&bad);
    {
        static const char keys[] = "titlekek_00 = 202122232425262728292a2b2c2d2e2f\n";
        Keys_Parse(&bad, keys, sizeof(keys) - 1);
    }
    CHECK(Nsp_ExtractMain(&bad, nsp, nsp_size, scratch, sizeof(scratch), &out, &out_len) == NCA_ERR_NO_HEADER_KEY,
          "neg missing header key rejected");

    /* Direct header/ticket field checks. */
    CHECK(Pfs0_Parse(&pfs, nsp, nsp_size) == PFS0_OK, "neg nsp parses");
    {
        int ni = Pfs0_Find(&pfs, "program.nca");
        int ti = Pfs0_Find(&pfs, "program.tik");
        Nca nca;
        uint8_t enc[0x10], rev = 0xFF, rid[0x10];
        uint64_t sz;
        const uint8_t *d;
        CHECK(ni >= 0 && ti >= 0, "neg entries present");
        d = Pfs0_Data(&pfs, (uint32_t)ni, &sz);
        CHECK(Nca_Parse(&nca, d, sz, &k) == NCA_OK, "neg Nca_Parse");
        CHECK(nca.content_type == NCA_CONTENT_PROGRAM, "neg content type");
        CHECK(nca.has_rights_id, "neg rights id present");
        CHECK(nca.section[0].present && nca.section[0].fs_type == NCA_FS_PFS0, "neg section");
        CHECK(nca.section[0].crypt_type == NCA_CRYPT_CTR, "neg section crypt type");
        CHECK(nca.section[0].pfs0_offset == HASH_LEN, "neg pfs0 offset");
        d = Pfs0_Data(&pfs, (uint32_t)ti, &sz);
        CHECK(Nca_ParseTicket(d, sz, enc, &rev, rid) == NCA_OK, "neg ticket parse");
        CHECK(rev == 0, "neg ticket revision");
        CHECK(rid[0] == 0xC0 && rid[15] == 0xCF, "neg ticket rights id");
        CHECK(memcmp(rid, nca.rights_id, 16) == 0, "neg ticket matches nca rights id");
    }
}

int main(void) {
    test_crypto();
    test_keys();
    test_synthetic();
    test_header_layout();
    test_ticket_layout();
    test_negative();
    if (fails == 0) { printf("PASS: nca (crypto KATs, keys parser, layouts, synthetic NSP->NCA->ExeFS->NSO)\n"); return 0; }
    printf("FAIL: %d check(s) failed\n", fails);
    return 1;
}
