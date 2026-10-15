/* library.cpp: see library.h. */
#include <string.h>

#include "library.h"

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

    int tn = 0;
    int title_len = dot ? (int)(dot - name) : fn;
    for (; tn < title_len && tn < (int)sizeof(g->title) - 1; ++tn) g->title[tn] = name[tn];
    g->title[tn] = 0;

    int en = 0;
    for (; ext[en] && en < (int)sizeof(g->ext) - 1; ++en) g->ext[en] = ext[en];
    g->ext[en] = 0;

    g->size = size;
    g->supported = 0; /* no emulation core yet: detected, not playable */
    g->hue = HueFromName(g->file);
    g->art = 0;
    g->art_w = g->art_h = 0;
    g->art_tried = 0;

    l->count++;
    return 1;
}

int Library_SupportedCount(const Library *l) {
    if (!l) return 0;
    int n = 0;
    for (int i = 0; i < l->count; ++i) if (l->items[i].supported) ++n;
    return n;
}
