/* fat32.c: see fat32.h. */
#include <string.h>

#include "fat32.h"

#define SECSZ        512u
#define FAT32_BAD    0x0FFFFFF7u /* bad cluster; >= this terminates a chain */
#define ATTR_LFN     0x0Fu
#define ATTR_DIR     0x10u
#define ATTR_VOLUME  0x08u

static Fat32ReadSectors g_rd;
static void  *g_ctx;
static uint32_t g_spc;         /* sectors per cluster        */
static uint32_t g_fat_start;   /* FAT LBA                    */
static uint32_t g_fat_sz;      /* sectors per FAT            */
static uint32_t g_num_fats;
static uint32_t g_data_start;  /* first data LBA             */
static uint32_t g_root_clus;
static uint32_t g_clus_cnt;    /* data clusters, 0 if unknown */

static uint8_t g_sec[SECSZ];

static int ReadSec(uint32_t lba, void *buf) {
    return g_rd(g_ctx, lba, 1u, buf);
}

/* A chain terminator: end-of-chain markers, the bad-cluster marker, or any
 * cluster past the end of the volume. Guarding this here keeps a corrupt FAT
 * from ever producing a wild LBA (ClusterToLba would otherwise multiply a
 * garbage index straight into the disc). */
static int IsEnd(uint32_t clus) {
    if (clus >= FAT32_BAD) return 1;
    if (g_clus_cnt != 0 && clus >= 2u + g_clus_cnt) return 1;
    return 0;
}

static uint32_t ClusterToLba(uint32_t n) {
    return g_data_start + (n - 2u) * g_spc;
}

static uint32_t FatNext(uint32_t clus) {
    uint32_t off = clus * 4u;
    uint32_t lba = g_fat_start + off / SECSZ;
    uint32_t o   = off % SECSZ;
    if (!ReadSec(lba, g_sec)) return FAT32_BAD; /* stop the chain on I/O error */
    uint32_t v = (uint32_t)g_sec[o] | ((uint32_t)g_sec[o + 1] << 8) |
                 ((uint32_t)g_sec[o + 2] << 16) | ((uint32_t)g_sec[o + 3] << 24);
    return v & 0x0FFFFFFFu;
}

static int LooksFat32(uint32_t lba) {
    if (!ReadSec(lba, g_sec)) return 0;
    const uint8_t *b = g_sec;
    uint32_t bps = (uint32_t)b[11] | ((uint32_t)b[12] << 8);
    uint32_t spc = b[13];
    uint32_t root_ent = (uint32_t)b[17] | ((uint32_t)b[18] << 8);
    uint32_t fsz16 = (uint32_t)b[22] | ((uint32_t)b[23] << 8);
    if (bps != SECSZ || spc == 0) return 0;
    if (root_ent != 0 || fsz16 != 0) return 0; /* that's FAT12/16 */
    if (b[510] != 0x55 || b[511] != 0xAA) return 0;
    return 1;
}

static void ParseBpb(void) {
    const uint8_t *b = g_sec;
    uint32_t res = (uint32_t)b[14] | ((uint32_t)b[15] << 8);
    g_fat_sz = (uint32_t)b[36] | ((uint32_t)b[37] << 8) |
               ((uint32_t)b[38] << 16) | ((uint32_t)b[39] << 24);
    g_num_fats = b[16];
    g_spc = b[13];
    g_fat_start = res;
    g_data_start = res + g_num_fats * g_fat_sz;
    g_root_clus = (uint32_t)b[44] | ((uint32_t)b[45] << 8) |
                  ((uint32_t)b[46] << 16) | ((uint32_t)b[47] << 24);
    uint32_t tot32 = (uint32_t)b[32] | ((uint32_t)b[33] << 8) |
                     ((uint32_t)b[34] << 16) | ((uint32_t)b[35] << 24);
    if (tot32 > g_data_start)
        g_clus_cnt = (tot32 - g_data_start) / (g_spc ? g_spc : 1u);
    else
        g_clus_cnt = 0; /* malformed; fall back to no bound */
}

int Fat32_Mount(Fat32ReadSectors rd, void *ctx) {
    g_rd = rd;
    g_ctx = ctx;
    if (!rd) return 0;

    /* Superfloppy (VBR at LBA 0) or MBR partitioned? */
    uint32_t start = 0;
    if (!LooksFat32(0)) {
        if (!ReadSec(0, g_sec)) return 0;
        if (g_sec[510] != 0x55 || g_sec[511] != 0xAA) return 0;
        /* Try all four primary partition entries; FAT32 is not always first. */
        int found = 0;
        for (int i = 0; i < 4; ++i) {
            const uint8_t *pe = g_sec + 0x1BE + i * 16;
            if (pe[4] == 0) continue;                    /* empty slot     */
            uint32_t s = (uint32_t)pe[8] | ((uint32_t)pe[9] << 8) |
                         ((uint32_t)pe[10] << 16) | ((uint32_t)pe[11] << 24);
            if (s == 0) continue;
            if (LooksFat32(s)) { start = s; found = 1; break; }
        }
        if (!found) return 0;
    }
    if (!ReadSec(start, g_sec)) return 0;
    ParseBpb();
    /* Rebase everything by the partition start. */
    g_fat_start += start;
    g_data_start += start;
    if (g_spc == 0 || g_num_fats == 0 || g_fat_sz == 0) return 0;
    if (g_fat_start == start || g_root_clus < 2u) return 0; /* reserved==0 or junk */
    return 1;
}

/* --- directory helpers ------------------------------------------------- */

static char ToUpper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

/* Compares a target path component against a short entry name (8.3). */
static int MatchShort(const uint8_t *e, const char *want) {
    char name[13];
    int n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; ++i) name[n++] = ToUpper((char)e[i]);
    if (e[8] != ' ') { name[n++] = '.'; for (int i = 8; i < 11 && e[i] != ' '; ++i)
                           name[n++] = ToUpper((char)e[i]); }
    name[n] = 0;
    const char *p = want;
    for (int i = 0; i < n; ++i) {
        if (!*p || ToUpper(*p) != name[i]) return 0;
        ++p;
    }
    return *p == 0;
}

/* Case-insensitive compare of an assembled LFN against the target. */
static int MatchLong(const char *lfn, const char *want) {
    for (;;) {
        if (ToUpper(*lfn) != ToUpper(*want)) return 0;
        if (!*lfn) return 1;
        ++lfn; ++want;
    }
}

static void LfnAppend13(char *lfn, int seq, const uint8_t *e) {
    static const int off[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    int base = (seq - 1) * 13;
    for (int i = 0; i < 13; ++i) {
        uint16_t c = (uint16_t)e[off[i]] | ((uint16_t)e[off[i] + 1] << 8);
        if (c == 0x0000 || c == 0xFFFF) { lfn[base + i] = 0; return; }
        lfn[base + i] = (c < 128) ? (char)c : '?';
    }
}

/* Finds `comp` in directory starting at cluster `dir_clus`.
 * On success returns 1 and sets *out_clus (file/dir start) and *out_size. */
static int FindEntry(uint32_t dir_clus, const char *comp,
                     uint32_t *out_clus, uint32_t *out_size, int *out_is_dir) {
    char lfn[261];
    memset(lfn, 0, sizeof(lfn));
    uint32_t clus = dir_clus;
    uint32_t guard = 0;
    while (clus >= 2u && !IsEnd(clus) && guard++ < 65536u) {
        uint32_t lba = ClusterToLba(clus);
        for (uint32_t s = 0; s < g_spc; ++s) {
            if (!ReadSec(lba + s, g_sec)) return 0;
            for (uint32_t o = 0; o < SECSZ; o += 32u) {
                const uint8_t *e = g_sec + o;
                if (e[0] == 0x00) return 0;         /* end of dir */
                if (e[0] == 0xE5) { lfn[0] = 0; continue; }
                uint8_t attr = e[11];
                if (attr == ATTR_LFN) {
                    int seq = e[0] & 0x3F;
                    if (seq >= 1 && seq <= 20) LfnAppend13(lfn, seq, e);
                    continue;
                }
                if (attr & ATTR_VOLUME) { lfn[0] = 0; continue; }
                int ok = 0;
                if (lfn[0]) ok = MatchLong(lfn, comp);
                if (!ok && MatchShort(e, comp)) ok = 1;
                if (ok) {
                    uint32_t hi = (uint32_t)e[20] | ((uint32_t)e[21] << 8);
                    uint32_t lo = (uint32_t)e[26] | ((uint32_t)e[27] << 8);
                    *out_clus = (hi << 16) | lo;
                    *out_size = (uint32_t)e[28] | ((uint32_t)e[29] << 8) |
                                ((uint32_t)e[30] << 16) | ((uint32_t)e[31] << 24);
                    *out_is_dir = (attr & ATTR_DIR) ? 1 : 0;
                    return 1;
                }
                lfn[0] = 0;
            }
        }
        clus = FatNext(clus);
    }
    return 0;
}

long Fat32_ReadFile(const char *path, void *buf, uint32_t maxlen) {
    if (!path || !buf) return -1;
    /* Skip a leading '/' or "0:/" style prefix. */
    while (*path == '/') ++path;

    uint32_t clus = g_root_clus;
    int is_dir = 1;
    const char *p = path;
    while (*p) {
        char comp[128];
        int n = 0;
        while (*p && *p != '/' && n < 127) comp[n++] = *p++;
        comp[n] = 0;
        if (*p == '/') ++p;

        uint32_t fc = 0, fs = 0; int fd = 0;
        if (!is_dir) return -1;
        if (!FindEntry(clus, comp, &fc, &fs, &fd)) return -1;
        clus = fc; is_dir = fd;
        if (*p == 0) { /* last component = the file */
            if (fd) return -1;
            uint32_t left = fs; if (left > maxlen) left = maxlen;
            uint32_t got = 0;
            uint32_t c = clus; uint32_t guard = 0;
            while (c >= 2u && !IsEnd(c) && got < left && guard++ < 65536u) {
                uint32_t lba = ClusterToLba(c);
                for (uint32_t s = 0; s < g_spc && got < left; ++s) {
                    if (!ReadSec(lba + s, g_sec)) return -1;
                    uint32_t chunk = left - got; if (chunk > SECSZ) chunk = SECSZ;
                    memcpy((uint8_t *)buf + got, g_sec, chunk);
                    got += chunk;
                }
                c = FatNext(c);
            }
            return (long)got;
        }
    }
    return -1;
}
