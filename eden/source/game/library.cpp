/* library.cpp: see library.h. Filename + container-header metadata parsing. */
#include <string.h>

#include "loader.h"
#include "library.h"

/* ---- small string helpers -------------------------------------------- */

static int ExtMatch(const char *ext, const char *want) {
    for (;; ++ext, ++want) {
        char a = *ext, b = *want;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
        if (!a) return 1;
    }
}

int Library_IsGameExt(const char *ext) {
    if (!ext) return 0;
    return ExtMatch(ext, ".nsp") || ExtMatch(ext, ".xci") ||
           ExtMatch(ext, ".nro") || ExtMatch(ext, ".nso");
}

/* Finds the last '.' of the basename (ignoring a leading dot). */
static const char *FindExt(const char *name) {
    const char *dot = 0;
    for (const char *p = name; *p; ++p) {
        if (*p == '.') dot = p;
        if (*p == '/' || *p == '\\') dot = 0; /* reset at path separators */
    }
    if (dot == name) return 0; /* a bare ".hidden" has no extension */
    return dot;
}

static void CopyLowerExt(char *dst, int dstsz, const char *dot) {
    int i = 0;
    for (; dot[i] && i < dstsz - 1; ++i) {
        char c = dot[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        dst[i] = c;
    }
    dst[i] = 0;
}

static uint16_t HueFromName(const char *name) {
    uint32_t h = 2166136261u; /* FNV-1a */
    for (const char *p = name; *p; ++p) {
        h ^= (unsigned char)*p;
        h *= 16777619u;
    }
    return (uint16_t)(h % 360u);
}

static char UpperCh(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    return c;
}

static int IsHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static int IsUpperAlpha(char c) { return c >= 'A' && c <= 'Z'; }

static void CopyN(char *dst, int dstsz, const char *src, int n) {
    int i = 0;
    for (; i < n && i < dstsz - 1; ++i) dst[i] = src[i];
    dst[i] = 0;
}

/* ---- filename tag parsing -------------------------------------------- */

void Library_ParseName(const char *name, Game *g) {
    if (!name || !g) return;

    const char *dot = FindExt(name);
    const char *bracket = strchr(name, '[');

    /* Display title = text before the first "[tag]"; if none, the basename. */
    const char *title_end = bracket ? bracket : (dot ? dot : name + strlen(name));
    int tlen = (int)(title_end - name);
    while (tlen > 0 && (name[tlen - 1] == ' ' || name[tlen - 1] == '_')) --tlen;
    CopyN(g->title, (int)sizeof(g->title), name, tlen);

    /* Walk the [tag][tag]... sequence. */
    const char *p = bracket;
    while (p && *p == '[') {
        const char *end = strchr(p, ']');
        if (!end) break;
        const char *inner = p + 1;
        int n = (int)(end - inner);

        if (n == 16 && !g->title_id[0]) {
            int allhex = 1;
            for (int i = 0; i < 16; ++i) if (!IsHex(inner[i])) { allhex = 0; break; }
            if (allhex) {
                for (int i = 0; i < 16; ++i) g->title_id[i] = UpperCh(inner[i]);
                g->title_id[16] = 0;
            }
        } else if ((inner[0] == 'v' || inner[0] == 'V') && n >= 2 && n <= 8 &&
                   !g->version[0]) {
            int ok = 1;
            for (int i = 1; i < n; ++i)
                if (!((inner[i] >= '0' && inner[i] <= '9') || inner[i] == '.'))
                    { ok = 0; break; }
            if (ok) {
                g->version[0] = 'v';
                CopyN(g->version + 1, (int)sizeof(g->version) - 1, inner + 1, n - 1);
            }
        } else if (n >= 2 && n <= 8 && !g->region[0]) {
            int allupper = 1;
            for (int i = 0; i < n; ++i) if (!IsUpperAlpha(inner[i])) { allupper = 0; break; }
            if (allupper) CopyN(g->region, (int)sizeof(g->region), inner, n);
        }

        p = end + 1;
        while (*p == ' ') ++p;
    }
}

/* ---- container header parsing ---------------------------------------- */

static uint32_t Rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int EndsWith(const char *s, int n, const char *suf) {
    int m = (int)strlen(suf);
    if (n < m) return 0;
    for (int i = 0; i < m; ++i) {
        char a = s[n - m + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return 0;
    }
    return 1;
}

/* NSP/PFS0: header + entry table + NUL-separated string table.
 * The .tik / .cert entry name starts with the 16-hex title ID. */
static void ScanPfs0(const unsigned char *b, uint32_t len, Game *g) {
    g->container = CONT_NSP;
    if (len < 0x10) return;

    uint32_t num = Rd32(b + 4);
    uint32_t strsz = Rd32(b + 8);
    if (num > 4096) num = 4096;

    uint32_t ent = 0x10;
    uint64_t entsz = (uint64_t)num * 0x18;
    if ((uint64_t)ent + entsz > len) return;
    const unsigned char *st = b + ent + (uint32_t)entsz;
    uint32_t stavail = len - (uint32_t)(ent + entsz);
    if (strsz < stavail) stavail = strsz; /* trust the declared table size */

    int count = 0;
    for (uint32_t i = 0; i < num; ++i) {
        const unsigned char *e = b + ent + i * 0x18;
        uint32_t soff = Rd32(e + 0x10);
        if (soff >= stavail) continue;
        const char *name = (const char *)(st + soff);
        uint32_t maxl = stavail - soff;
        uint32_t l = 0;
        while (l < maxl && name[l]) ++l;
        if (l == maxl) continue; /* not NUL-terminated: truncated entry */
        if (l == 0) continue;
        count++;

        if (EndsWith(name, (int)l, ".tik") || EndsWith(name, (int)l, ".cert")) {
            int k = 0;
            char tid[GAME_TITLEID_LEN];
            while (k < 16 && IsHex(name[k])) { tid[k] = UpperCh(name[k]); k++; }
            if (k == 16) {
                tid[16] = 0;
                CopyN(g->title_id, (int)sizeof(g->title_id), tid, 16);
                g->title_id_from_header = 1;
            }
        }
    }
    g->content_count = count;
    g->meta_scanned = 1;
}

int Library_ScanContainer(const void *data, uint32_t len, Game *g) {
    if (!data || !g) return CONT_UNKNOWN;
    const unsigned char *b = (const unsigned char *)data;

    if (len >= 4 && b[0] == 'P' && b[1] == 'F' && b[2] == 'S' && b[3] == '0') {
        ScanPfs0(b, len, g);
        return g->container;
    }
    if (len >= 0x104 && memcmp(b + 0x100, "HEAD", 4) == 0) {
        g->container = CONT_XCI;
        g->meta_scanned = 1;
        return CONT_XCI;
    }
    if (len >= 4 && b[0] == 'N' && b[1] == 'R' && b[2] == 'O' && b[3] == '0') {
        g->container = CONT_NRO;
        Loader_ProbeModule(b, len, &g->mod);
        g->meta_scanned = 1;
        return CONT_NRO;
    }
    if (len >= 4 && b[0] == 'N' && b[1] == 'S' && b[2] == 'O' && b[3] == '0') {
        g->container = CONT_NSO;
        Loader_ProbeModule(b, len, &g->mod);
        g->meta_scanned = 1;
        return CONT_NSO;
    }

    g->container = CONT_UNKNOWN;
    g->meta_scanned = 1;
    return CONT_UNKNOWN;
}

const char *Library_ContainerName(int c) {
    switch (c) {
    case CONT_NSP: return "NSP (PFS0)";
    case CONT_XCI: return "XCI (gamecard)";
    case CONT_NRO: return "NRO (homebrew)";
    case CONT_NSO: return "NSO (binary)";
    default:       return "unknown";
    }
}

/* ---- library management ---------------------------------------------- */

void Library_Init(Library *l) {
    if (!l) return;
    memset(l, 0, sizeof(*l));
}

void Library_Clear(Library *l) {
    if (!l) return;
    memset(l, 0, sizeof(*l));
}

int Library_AddFile(Library *l, const char *name, uint32_t size) {
    if (!l || !name) return 0;
    l->files_seen++;

    const char *dot = FindExt(name);
    char ext[8];
    CopyLowerExt(ext, (int)sizeof(ext), dot ? dot : "");
    if (!Library_IsGameExt(ext)) { l->rejected++; return 0; }

    if (l->count >= GAME_MAX) return 0;

    Game *g = &l->items[l->count];
    memset(g, 0, sizeof(*g));

    int fn = 0;
    for (; name[fn] && fn < (int)sizeof(g->file) - 1; ++fn) g->file[fn] = name[fn];
    g->file[fn] = 0;

    int en = 0;
    for (; ext[en] && en < (int)sizeof(g->ext) - 1; ++en) g->ext[en] = ext[en];
    g->ext[en] = 0;

    g->size = size;
    g->supported = 0; /* no emulation core yet: detected, not playable */
    g->hue = HueFromName(g->file);

    Library_ParseName(name, g);

    l->count++;
    return 1;
}

int Library_SupportedCount(const Library *l) {
    if (!l) return 0;
    int n = 0;
    for (int i = 0; i < l->count; ++i) if (l->items[i].supported) ++n;
    return n;
}
