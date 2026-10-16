/* keys.c: see keys.h. */
#include "keys.h"

#include <string.h>

/* Trim leading/trailing whitespace in [*p, *end); advances *p and shrinks *end. */
static void trim(const char **p, const char **end) {
    while (*p < *end && ((**p == ' ' || **p == '\t' || **p == '\r'))) ++*p;
    while (*end > *p && ((*end)[-1] == ' ' || (*end)[-1] == '\t' || (*end)[-1] == '\r')) --*end;
}

static int hexdigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse a hex string of even length into out[0..cap). Returns byte count, or
 * -1 on malformed input / overflow. */
static int parse_hex(const char *s, const char *end, uint8_t *out, size_t cap) {
    size_t n = (size_t)(end - s);
    size_t i;
    if (n == 0 || (n & 1)) return -1;
    if (n / 2 > cap) return -1;
    for (i = 0; i < n; i += 2) {
        int h = hexdigit(s[i]);
        int l = hexdigit(s[i + 1]);
        if (h < 0 || l < 0) return -1;
        out[i / 2] = (uint8_t)((h << 4) | l);
    }
    return (int)(n / 2);
}

/* Parse a small non-negative hex revision suffix like "00".."1f". */
static int parse_revision(const char *s, const char *end, unsigned *out) {
    unsigned v = 0;
    int n = 0;
    while (s < end) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else return 0;
        v = v * 16u + (unsigned)d;
        if (++n > 4) return 0;
        ++s;
    }
    *out = v;
    return n > 0;
}

static int name_eq(const char *a, const char *aend, const char *b) {
    size_t n = (size_t)(aend - a);
    return strlen(b) == n && memcmp(a, b, n) == 0;
}

static int name_has_prefix(const char *a, const char *aend, const char *pfx, const char **rest, const char **rend) {
    size_t n = strlen(pfx);
    if ((size_t)(aend - a) <= n) return 0;
    if (memcmp(a, pfx, n) != 0) return 0;
    *rest = a + n;
    *rend = aend;
    return 1;
}

void Keys_Init(ProdKeys *k) {
    if (k) {
        size_t n = sizeof(*k);
        uint8_t *p = (uint8_t *)k;
        while (n--) *p++ = 0;
    }
}

const char *Keys_ResultName(KeysResult r) {
    switch (r) {
        case KEYS_OK:       return "ok";
        case KEYS_ERR_IO:   return "i/o error";
        case KEYS_ERR_NONE: return "no usable keys";
        default:            return "unknown";
    }
}

KeysResult Keys_Parse(ProdKeys *k, const void *data, size_t len) {
    const char *s = (const char *)data;
    const char *p = s;
    const char *end = s + len;

    if (!k) return KEYS_ERR_NONE;

    while (p < end) {
        const char *line_end = p;
        const char *eq;
        const char *name, *name_end, *val, *val_end;
        uint8_t valbuf[KEYS_MAX_VALUE];
        int vlen;
        const char *rest, *rend;
        unsigned rev;

        while (line_end < end && *line_end != '\n') ++line_end;

        /* Trim the whole line. */
        {
            const char *ls = p, *le = line_end;
            trim(&ls, &le);
            /* Skip blank lines and comments. */
            if (ls >= le || *ls == '#') { p = line_end + 1; continue; }

            /* Split at the first '='. */
            eq = ls;
            while (eq < le && *eq != '=') ++eq;
            if (eq >= le) { p = line_end + 1; continue; }

            name = ls; name_end = eq;
            trim(&name, &name_end);
            val = eq + 1; val_end = le;
            /* value stops at trailing comment '#' */
            {
                const char *h = val;
                while (h < val_end && *h != '#') ++h;
                val_end = h;
            }
            trim(&val, &val_end);
        }

        vlen = parse_hex(val, val_end, valbuf, sizeof(valbuf));
        if (vlen > 0) {
            if (name_eq(name, name_end, "header_key")) {
                if (vlen == 16 || vlen == 32) {
                    uint32_t i;
                    for (i = 0; i < (uint32_t)vlen; ++i) k->header_key[i] = valbuf[i];
                    k->header_key_len = (uint32_t)vlen;
                    k->have_header_key = 1;
                    k->count++;
                }
            } else if (vlen == 16 &&
                       name_has_prefix(name, name_end, "titlekek_", &rest, &rend) &&
                       parse_revision(rest, rend, &rev) && rev < KEYS_MAX_REVISION) {
                uint32_t i;
                for (i = 0; i < 16; ++i) k->titlekek[rev][i] = valbuf[i];
                k->have_titlekek[rev] = 1;
                k->count++;
            } else if (vlen == 16) {
                int type = -1;
                if (name_has_prefix(name, name_end, "key_area_key_application_", &rest, &rend)) type = KEYS_KAEK_APPLICATION;
                else if (name_has_prefix(name, name_end, "key_area_key_ocean_", &rest, &rend)) type = KEYS_KAEK_OCEAN;
                else if (name_has_prefix(name, name_end, "key_area_key_system_", &rest, &rend)) type = KEYS_KAEK_SYSTEM;
                if (type >= 0 && parse_revision(rest, rend, &rev) && rev < KEYS_MAX_REVISION) {
                    uint32_t i;
                    for (i = 0; i < 16; ++i) k->kaek[type][rev][i] = valbuf[i];
                    k->have_kaek[type][rev] = 1;
                    k->count++;
                }
            }
        }

        p = line_end + 1;
    }

    return k->count ? KEYS_OK : KEYS_ERR_NONE;
}

const uint8_t *Keys_HeaderKey(const ProdKeys *k, uint32_t *out_len) {
    if (!k || !k->have_header_key) return NULL;
    if (out_len) *out_len = k->header_key_len;
    return k->header_key;
}

const uint8_t *Keys_Titlekek(const ProdKeys *k, unsigned revision) {
    if (!k || revision >= KEYS_MAX_REVISION || !k->have_titlekek[revision]) return NULL;
    return k->titlekek[revision];
}

const uint8_t *Keys_Kaek(const ProdKeys *k, int kaek_type, unsigned revision) {
    if (!k || kaek_type < 0 || kaek_type >= KEYS_KAEK_COUNT) return NULL;
    if (revision >= KEYS_MAX_REVISION || !k->have_kaek[kaek_type][revision]) return NULL;
    return k->kaek[kaek_type][revision];
}

#ifdef __cplusplus
}
#endif
