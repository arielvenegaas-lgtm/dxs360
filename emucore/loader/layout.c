/* layout.c: kernel-style multi-module layout (see layout.h). */
#include "layout.h"

#include <string.h>

static uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

/* Case-sensitive compare against "<prefix><digit><NUL>" for a single digit. */
static int match_prefixed_number(const char *name, const char *prefix, int *out) {
    size_t pl = strlen(prefix);
    if (strncmp(name, prefix, pl) != 0) return 0;
    if (name[pl] >= '0' && name[pl] <= '9' && name[pl + 1] == '\0') {
        *out = name[pl] - '0';
        return 1;
    }
    return 0;
}

int Layout_Rank(const char *name) {
    char buf[16];
    size_t len;
    int d;

    if (!name || !name[0]) return -1;

    /* Accept an explicit ".nso" suffix (some dumps keep it). */
    len = strlen(name);
    if (len >= 4 && strcmp(name + len - 4, ".nso") == 0) {
        if (len - 4 >= sizeof(buf)) return -1;
        memcpy(buf, name, len - 4);
        buf[len - 4] = '\0';
        name = buf;
    }

    if (strcmp(name, "rtld") == 0) return 0;
    if (strcmp(name, "main") == 0) return 1;
    if (strcmp(name, "sdk") == 0) return 22;
    if (match_prefixed_number(name, "wkc", &d)) return 2 + d;      /* wkc0..9  */
    if (match_prefixed_number(name, "subsdk", &d)) return 12 + d;  /* subsdk0..9 */
    return -1;
}

static uint32_t seg_prot(int idx) {
    switch (idx) {
        case LAYOUT_SEG_TEXT: return LAYOUT_PROT_READ | LAYOUT_PROT_EXEC;
        case LAYOUT_SEG_RO:   return LAYOUT_PROT_READ;
        case LAYOUT_SEG_DATA: return LAYOUT_PROT_READ | LAYOUT_PROT_WRITE;
        case LAYOUT_SEG_BSS:  return LAYOUT_PROT_READ | LAYOUT_PROT_WRITE;
        default:              return 0;
    }
}

/* True if [a0,a1) and [b0,b1) overlap (zero-length ranges never overlap). */
static int ranges_overlap(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1) {
    if (a0 == a1 || b0 == b1) return 0;
    return a0 < b1 && b0 < a1;
}

int Layout_Plan(const LayoutInput *in, int n, uint64_t code_address,
                LayoutPlan *out) {
    LayoutModule tmp[LAYOUT_MAX_MODULES];
    int i, j, count = 0;
    uint64_t base;

    if (!in || n <= 0 || !out) return 0;

    /* Collect the loadable modules (rank >= 0) with a non-zero image size. */
    for (i = 0; i < n; ++i) {
        int rank = Layout_Rank(in[i].name);
        if (rank < 0) continue;
        if (in[i].image_size == 0) continue;
        if (count >= LAYOUT_MAX_MODULES) return 0;
        memset(&tmp[count], 0, sizeof(tmp[count]));
        {
            size_t nl = strlen(in[i].name);
            if (nl >= sizeof(tmp[count].name)) nl = sizeof(tmp[count].name) - 1;
            memcpy(tmp[count].name, in[i].name, nl);
            tmp[count].name[nl] = '\0';
        }
        tmp[count].rank = rank;
        count++;
    }
    if (count == 0) return 0;

    /* Order by NsoIndex (rtld first). Insertion sort: count is tiny. */
    for (i = 1; i < count; ++i) {
        LayoutModule key = tmp[i];
        j = i - 1;
        while (j >= 0 && tmp[j].rank > key.rank) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = key;
    }

    /* Assign page-aligned contiguous bases and resolve every segment. */
    memset(out, 0, sizeof(*out));
    out->code_address = code_address;
    out->entry = code_address;
    base = code_address;

    for (i = 0; i < count; ++i) {
        LayoutModule *m = &tmp[i];
        const LayoutInput *src = NULL;
        uint64_t aligned, span_end;

        /* Find the matching input (first by name) for the geometry. */
        for (j = 0; j < n; ++j) {
            if (strcmp(in[j].name, m->name) == 0) { src = &in[j]; break; }
        }
        if (!src) return 0;

        aligned = align_up(src->image_size, LAYOUT_PAGE);
        if (aligned < src->image_size) return 0;           /* overflow */
        span_end = base + aligned;
        if (span_end < base) return 0;                     /* overflow */

        m->base = base;
        m->size = aligned;

        /* text, rodata, data segments (bss is folded into data's span). */
        for (j = 0; j < 3; ++j) {
            uint64_t saddr = base + src->seg_loc[j];
            uint64_t send  = saddr + src->seg_size[j];
            if (saddr < base || send < saddr) return 0;     /* overflow */
            m->seg[j].addr = saddr;
            m->seg[j].size = src->seg_size[j];
            m->seg[j].prot = seg_prot(j);
            /* A segment must fit inside the module's page-aligned span. */
            if (send > span_end) return 0;
        }
        {
            uint64_t baddr = m->seg[LAYOUT_SEG_DATA].addr +
                             m->seg[LAYOUT_SEG_DATA].size;
            uint64_t bend = baddr + src->bss_size;
            if (bend < baddr) return 0;
            m->seg[LAYOUT_SEG_BSS].addr = baddr;
            m->seg[LAYOUT_SEG_BSS].size = src->bss_size;
            m->seg[LAYOUT_SEG_BSS].prot = seg_prot(LAYOUT_SEG_BSS);
            if (bend > span_end) return 0;
        }

        /* Malformed headers would self-overlap; reject rather than corrupt. */
        if (ranges_overlap(m->seg[0].addr, m->seg[0].addr + m->seg[0].size,
                           m->seg[1].addr, m->seg[1].addr + m->seg[1].size) ||
            ranges_overlap(m->seg[0].addr, m->seg[0].addr + m->seg[0].size,
                           m->seg[2].addr, m->seg[2].addr + m->seg[2].size) ||
            ranges_overlap(m->seg[1].addr, m->seg[1].addr + m->seg[1].size,
                           m->seg[2].addr, m->seg[2].addr + m->seg[2].size))
            return 0;

        base = span_end;
    }

    memcpy(out->mod, tmp, sizeof(tmp[0]) * (size_t)count);
    out->count = count;
    out->total_size = base - code_address;
    return 1;
}
