/* nca.c: see nca.h. */
#include "nca.h"

#include <string.h>

#include "aes.h"
#include "pfs0.h"

static uint64_t rd_le64(const uint8_t *p) {
    uint64_t v = 0;
    int i;
    for (i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t bswap32(uint32_t x) {
    return ((x & 0xFFu) << 24) | ((x & 0xFF00u) << 8) |
           ((x >> 8) & 0xFF00u) | ((x >> 24) & 0xFFu);
}

const char *Nca_ResultName(NcaResult r) {
    switch (r) {
        case NCA_OK:              return "ok";
        case NCA_ERR_TOO_SMALL:   return "buffer too small";
        case NCA_ERR_BAD_MAGIC:   return "bad magic (not NCA3)";
        case NCA_ERR_NO_HEADER_KEY: return "missing header_key";
        case NCA_ERR_RANGE:       return "offset out of range";
        case NCA_ERR_UNSUPPORTED: return "unsupported";
        case NCA_ERR_NO_SECTION:  return "no section";
        case NCA_ERR_NO_NSO:      return "no 'main' NSO in ExeFS";
        case NCA_ERR_NEED_KEY:    return "title key required";
        case NCA_ERR_NO_KEYS:     return "required key missing";
        default:                  return "unknown";
    }
}

NcaResult Nca_Parse(Nca *nca, const void *data, uint64_t size, const ProdKeys *keys) {
    const uint8_t *in = (const uint8_t *)data;
    uint8_t hdr[NCA_HEADER_SIZE];
    uint32_t hk_len = 0;
    const uint8_t *hk;
    int i;

    if (!nca || !data) return NCA_ERR_RANGE;
    if (size < NCA_HEADER_SIZE) return NCA_ERR_TOO_SMALL;

    hk = Keys_HeaderKey(keys, &hk_len);
    if (!hk || hk_len != 32) return NCA_ERR_NO_HEADER_KEY;

    Aes_XtsDecryptNca(hk, hdr, in, NCA_HEADER_SIZE, 0);

    memset(nca, 0, sizeof(*nca));
    nca->magic = rd_le32(hdr + 0x200);
    if (nca->magic != 0x3341434E /* "NCA3" */ &&
        nca->magic != 0x3241434E /* "NCA2" */) return NCA_ERR_BAD_MAGIC;

    nca->distribution  = hdr[0x204];
    nca->content_type  = hdr[0x205];
    nca->crypto_type   = hdr[0x206];
    nca->kaek_ind      = hdr[0x207];
    nca->nca_size      = rd_le64(hdr + 0x208);
    nca->title_id      = rd_le64(hdr + 0x210);
    nca->crypto_type2  = hdr[0x220];
    nca->key_generation = (nca->crypto_type > nca->crypto_type2)
                              ? nca->crypto_type : nca->crypto_type2;
    if (nca->key_generation) nca->key_generation--;

    memcpy(nca->rights_id, hdr + 0x230, 0x10);
    nca->has_rights_id = 0;
    for (i = 0; i < 0x10; ++i) if (nca->rights_id[i]) { nca->has_rights_id = 1; break; }

    memcpy(nca->encrypted_key_area, hdr + 0x300, NCA_KEY_AREA_SIZE);

    for (i = 0; i < 4; ++i) {
        const uint8_t *se = hdr + 0x240 + (uint64_t)i * 0x10;
        uint32_t ms = rd_le32(se);
        uint32_t me = rd_le32(se + 4);
        const uint8_t *fh = hdr + 0x400 + (uint64_t)i * NCA_FS_HEADER_SIZE;
        NcaSection *s = &nca->section[i];

        if (ms == 0 || me <= ms) { s->present = 0; continue; }
        s->present = 1;
        s->partition_type = fh[0x2];
        s->fs_type        = fh[0x3];
        s->crypt_type     = fh[0x4];
        memcpy(s->section_ctr, fh + 0x140, 8);
        s->offset = (uint64_t)ms * NCA_MEDIA_UNIT;
        s->size   = (uint64_t)(me - ms) * NCA_MEDIA_UNIT;

        /* PFS0 superblock fields (valid when fs_type == PFS0). */
        s->pfs0_offset = rd_le64(fh + 0x8 + 0x38);
        s->pfs0_size   = rd_le64(fh + 0x8 + 0x40);
    }

    return NCA_OK;
}

NcaResult Nca_DecryptKeyArea(const Nca *nca, const ProdKeys *keys, uint8_t out[NCA_KEY_AREA_SIZE]) {
    const uint8_t *kaek;
    if (!nca || !keys || !out) return NCA_ERR_RANGE;
    kaek = Keys_Kaek(keys, nca->kaek_ind, nca->key_generation);
    if (!kaek) return NCA_ERR_NO_KEYS;
    Aes_EcbDecrypt(kaek, 16, out, nca->encrypted_key_area, NCA_KEY_AREA_SIZE);
    return NCA_OK;
}

NcaResult Nca_SectionKey(const Nca *nca, int i, const ProdKeys *keys,
                         const uint8_t *titlekey, uint8_t *key, size_t *key_len) {
    const NcaSection *s;
    if (!nca || !key || !key_len || i < 0 || i >= 4) return NCA_ERR_RANGE;
    s = &nca->section[i];
    if (!s->present) return NCA_ERR_NO_SECTION;

    if (nca->has_rights_id) {
        if (!titlekey) return NCA_ERR_NEED_KEY;
        memcpy(key, titlekey, 16);
        *key_len = 16;
        return NCA_OK;
    }

    if (s->crypt_type == NCA_CRYPT_NONE) { *key_len = 0; return NCA_OK; }

    if (s->crypt_type == NCA_CRYPT_XTS || s->crypt_type == NCA_CRYPT_CTR) {
        uint8_t dec[NCA_KEY_AREA_SIZE];
        NcaResult r = Nca_DecryptKeyArea(nca, keys, dec);
        if (r != NCA_OK) return r;
        if (s->crypt_type == NCA_CRYPT_XTS) {
            memcpy(key, dec, 32);
            *key_len = 32;
        } else {
            memcpy(key, dec + 0x20, 16);
            *key_len = 16;
        }
        return NCA_OK;
    }

    return NCA_ERR_UNSUPPORTED;
}

NcaResult Nca_DecryptSection(const Nca *nca, int i, const uint8_t *in,
                             uint8_t *out, const uint8_t *key, size_t key_len) {
    const NcaSection *s;
    if (!nca || !in || !out || i < 0 || i >= 4) return NCA_ERR_RANGE;
    s = &nca->section[i];
    if (!s->present) return NCA_ERR_NO_SECTION;

    if (key_len == 0) {
        memcpy(out, in, (size_t)s->size);
        return NCA_OK;
    }
    if (key_len == 32) { /* AES-XTS with section-relative 0-based sectors */
        uint64_t whole = s->size & ~(uint64_t)(NCA_MEDIA_UNIT - 1);
        if (whole) Aes_XtsDecryptNca(key, out, in, (size_t)whole, 0);
        if (s->size > whole) memcpy(out + whole, in + whole, (size_t)(s->size - whole));
        return NCA_OK;
    }
    if (key_len == 16) { /* AES-CTR; counter carries the absolute file offset */
        uint8_t ctr[16];
        uint64_t ofs = s->offset >> 4;
        int j;
        memset(ctr, 0, sizeof(ctr));
        for (j = 0; j < 8; ++j) ctr[j] = s->section_ctr[7 - j];
        for (j = 0; j < 8; ++j) { ctr[15 - j] = (uint8_t)(ofs & 0xFF); ofs >>= 8; }
        Aes_CtrXcrypt(key, 16, ctr, out, in, (size_t)s->size);
        return NCA_OK;
    }
    return NCA_ERR_UNSUPPORTED;
}

/* Ticket signature type -> signature size + trailing padding. The type is
 * stored little-endian on the Switch (nxtik / NSC_BUILDER); a big-endian value
 * is also accepted defensively. ECDSA carries a full 0x40 pad, so this is not a
 * plain align-to-0x40 and must be tabulated. */
static int ticket_sig_info(uint32_t type, uint32_t *sig_size, uint32_t *pad) {
    switch (type) {
        case 0x010000: case 0x010003: *sig_size = 0x200; *pad = 0x3C; return 1; /* RSA-4096 */
        case 0x010001: case 0x010004: *sig_size = 0x100; *pad = 0x3C; return 1; /* RSA-2048 */
        case 0x010002: case 0x010005: *sig_size = 0x3C;  *pad = 0x40; return 1; /* ECDSA    */
        case 0x010006:                *sig_size = 0x14;  *pad = 0x28; return 1; /* HMAC     */
        default: return 0;
    }
}

/* Minimum ticket-data length that holds every field the loader reads: issuer
 * (0x40), title-key block (0x100), the version/type/revision fields and the
 * rights_id (ending at 0x170) plus account_id (0x174). The ES spec pads
 * ticket data out to 0x2C0, but real-world NSPs routinely trim the trailing
 * "unknown" region; requiring the full 0x2C0 would reject valid tickets. */
#define NCA_TICKET_MIN_DATA 0x174

/* Compute Y, the size of the ticket's signature-data block. */
static int ticket_prefix(const uint8_t *t, uint64_t size, uint64_t *y) {
    uint32_t ss, pad, raw;
    if (size < 4) return 0;
    raw = rd_le32(t);
    if (!ticket_sig_info(raw, &ss, &pad) &&
        !ticket_sig_info(bswap32(raw), &ss, &pad)) return 0;
    *y = 4ull + (uint64_t)ss + (uint64_t)pad;
    return 1;
}

NcaResult Nca_ParseTicket(const void *ticket, uint64_t size,
                          uint8_t enc_titlekey[0x10], uint8_t *master_key_rev,
                          uint8_t rights_id[0x10]) {
    const uint8_t *t = (const uint8_t *)ticket;
    uint64_t y;

    if (!t) return NCA_ERR_RANGE;
    if (!ticket_prefix(t, size, &y)) return NCA_ERR_UNSUPPORTED;
    if (size < y + NCA_TICKET_MIN_DATA) return NCA_ERR_TOO_SMALL;

    if (enc_titlekey) memcpy(enc_titlekey, t + y + 0x40, 0x10);
    if (master_key_rev) *master_key_rev = t[y + 0x145];
    if (rights_id) memcpy(rights_id, t + y + 0x160, 0x10);
    return NCA_OK;
}

NcaResult Nca_TitleKey(const void *ticket, uint64_t size, const ProdKeys *keys,
                       uint8_t out_titlekey[0x10], uint8_t out_rights_id[0x10]) {
    const uint8_t *t = (const uint8_t *)ticket;
    uint64_t y;
    uint8_t enc[0x10];
    uint8_t key_type, rev;
    const uint8_t *kek;

    if (!t) return NCA_ERR_RANGE;
    if (!ticket_prefix(t, size, &y)) return NCA_ERR_UNSUPPORTED;
    if (size < y + NCA_TICKET_MIN_DATA) return NCA_ERR_TOO_SMALL;

    key_type = t[y + 0x141];
    rev = t[y + 0x145];
    if (key_type != 0) return NCA_ERR_UNSUPPORTED; /* personalized/RSA */

    memcpy(enc, t + y + 0x40, 0x10);
    kek = Keys_Titlekek(keys, rev);
    if (!kek) return NCA_ERR_NO_KEYS;

    Aes_EcbDecrypt(kek, 16, out_titlekey, enc, 0x10);
    if (out_rights_id) memcpy(out_rights_id, t + y + 0x160, 0x10);
    return NCA_OK;
}

NcaResult Nca_ExtractExefs(const Nca *nca, int i, const uint8_t *nca_data,
                           uint64_t nca_size, const ProdKeys *keys,
                           const uint8_t *titlekey, uint8_t *out, size_t out_cap,
                           const uint8_t **nso, uint64_t *nso_len) {
    const NcaSection *s;
    uint8_t key[32];
    size_t key_len = 0;
    NcaResult r;
    Pfs0 pfs;
    int mi;
    uint64_t nsz;
    const uint8_t *nd;

    if (!nca || !nca_data || !out || i < 0 || i >= 4) return NCA_ERR_RANGE;
    s = &nca->section[i];
    if (!s->present) return NCA_ERR_NO_SECTION;
    if (s->offset > nca_size || s->size > nca_size - s->offset) return NCA_ERR_RANGE;
    if (out_cap < s->size) return NCA_ERR_TOO_SMALL;

    r = Nca_SectionKey(nca, i, keys, titlekey, key, &key_len);
    if (r != NCA_OK) return r;

    r = Nca_DecryptSection(nca, i, nca_data + s->offset, out, key_len ? key : NULL, key_len);
    if (r != NCA_OK) return r;

    if (s->pfs0_offset > s->size || s->pfs0_size > s->size - s->pfs0_offset) return NCA_ERR_RANGE;
    if (Pfs0_Parse(&pfs, out + s->pfs0_offset, s->pfs0_size) != PFS0_OK) return NCA_ERR_RANGE;

    mi = Pfs0_Find(&pfs, "main");
    if (mi < 0) return NCA_ERR_NO_NSO;
    nd = Pfs0_Data(&pfs, (uint32_t)mi, &nsz);
    if (!nd) return NCA_ERR_NO_NSO;
    if (nso) *nso = nd;
    if (nso_len) *nso_len = nsz;
    return NCA_OK;
}

/* Locate the program NCA inside an NSP and its bytes. Returns NCA_OK on
 * success, NCA_ERR_NO_HEADER_KEY if the NCA headers can't be read at all, or
 * NCA_ERR_NO_SECTION otherwise. */
static NcaResult nsp_find_program_nca(const ProdKeys *keys, const Pfs0 *pfs,
                                      Nca *out_nca, const uint8_t **out_data, uint64_t *out_size) {
    uint32_t i;
    int saw_nca = 0;
    for (i = 0; i < pfs->count; ++i) {
        const char *name = Pfs0_Name(pfs, i);
        size_t nl = strlen(name);
        uint64_t sz;
        const uint8_t *d;
        Nca cand;
        NcaResult r;
        if (nl < 4 || strcmp(name + nl - 4, ".nca") != 0) continue;
        d = Pfs0_Data(pfs, i, &sz);
        if (!d) continue;
        saw_nca = 1;
        r = Nca_Parse(&cand, d, sz, keys);
        if (r == NCA_ERR_NO_HEADER_KEY) return r;
        if (r != NCA_OK) continue;
        if (cand.content_type != NCA_CONTENT_PROGRAM) continue;
        *out_nca = cand;
        *out_data = d;
        *out_size = sz;
        return NCA_OK;
    }
    (void)saw_nca;
    return NCA_ERR_NO_SECTION;
}

NcaResult Nsp_ExtractMain(const ProdKeys *keys, const uint8_t *nsp, uint64_t nsp_size,
                          uint8_t *scratch, size_t scratch_cap,
                          const uint8_t **nso, uint64_t *nso_len) {
    Pfs0 pfs;
    Nca nca;
    const uint8_t *nca_data = NULL;
    uint64_t nca_size = 0;
    uint8_t titlekey[16];
    const uint8_t *tk = NULL;
    uint32_t i;

    if (!keys || !nsp || !scratch || !nso || !nso_len) return NCA_ERR_RANGE;
    if (Pfs0_Parse(&pfs, nsp, nsp_size) != PFS0_OK) return NCA_ERR_RANGE;
    {
        NcaResult fr = nsp_find_program_nca(keys, &pfs, &nca, &nca_data, &nca_size);
        if (fr != NCA_OK) return fr;
    }

    if (nca.has_rights_id) {
        for (i = 0; i < pfs.count; ++i) {
            const char *name = Pfs0_Name(&pfs, i);
            size_t nl = strlen(name);
            uint64_t sz;
            const uint8_t *d;
            uint8_t rid[0x10];
            if (nl < 4 || strcmp(name + nl - 4, ".tik") != 0) continue;
            d = Pfs0_Data(&pfs, i, &sz);
            if (!d) continue;
            if (Nca_ParseTicket(d, sz, NULL, NULL, rid) != NCA_OK) continue;
            if (memcmp(rid, nca.rights_id, 0x10) != 0) continue;
            if (Nca_TitleKey(d, sz, keys, titlekey, NULL) != NCA_OK) continue;
            tk = titlekey;
            break;
        }
        if (!tk) return NCA_ERR_NEED_KEY;
    }

    for (i = 0; i < 4; ++i) {
        const NcaSection *s = &nca.section[i];
        if (!s->present || s->partition_type != NCA_PARTITION_PFS0 ||
            s->fs_type != NCA_FS_PFS0) continue;
        return Nca_ExtractExefs(&nca, (int)i, nca_data, nca_size, keys, tk,
                                scratch, scratch_cap, nso, nso_len);
    }
    return NCA_ERR_NO_SECTION;
}

#ifdef __cplusplus
}
#endif
