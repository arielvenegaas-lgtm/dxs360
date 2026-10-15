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

/* Parses the BPB at `start` into the globals. Returns 1 if it is a usable
 * FAT32 volume. */
static int MountPartition(uint32_t start) {
    if (!ReadSec(start, g_sec)) return 0;
    ParseBpb();
    /* Rebase everything by the partition start. */
    g_fat_start += start;
    g_data_start += start;
    if (g_spc == 0 || g_num_fats == 0 || g_fat_sz == 0) return 0;
    if (g_fat_start == start || g_root_clus < 2u) return 0; /* reserved==0 or junk */
    return 1;
}

/* True if the currently-mounted root contains a folder we care about. Used to
 * pick the right FAT32 partition on a multi-partition drive (very common: a
 * small empty FAT32 partition comes first, the data partition second). */
static int RootHasMarker(void) {
    static const char *m[] = { "roms", "ROMs", "keys", "games", "switch" };
    for (unsigned i = 0; i < sizeof(m) / sizeof(m[0]); ++i)
        if (Fat32_ListDir(m[i], 0, 0) != -1) return 1;
    return 0;
}

#define FAT32_MAX_CAND 16

/* Collects FAT32 partition start LBAs: all four primary entries, then any
 * extended (0x05/0x0F/0x85) partition's logical volumes via the EBR chain. */
static int CollectPartitions(uint32_t *out, int max) {
    int n = 0;
    uint8_t tbl[SECSZ];
    /* Copy the table first: LooksFat32() reuses g_sec, so we must not read
     * partition entries out of it after the first probe. */
    if (!g_rd(g_ctx, 0, 1, tbl)) return 0;
    if (tbl[510] != 0x55 || tbl[511] != 0xAA) return 0;

    for (int i = 0; i < 4; ++i) {
        const uint8_t *pe = tbl + 0x1BE + i * 16;
        uint8_t type = pe[4];
        if (type == 0) continue;
        uint32_t s = (uint32_t)pe[8] | ((uint32_t)pe[9] << 8) |
                     ((uint32_t)pe[10] << 16) | ((uint32_t)pe[11] << 24);
        if (s == 0) continue;
        if (type == 0x05 || type == 0x0F || type == 0x85) {
            /* Walk the extended partition's EBR chain. Offsets are relative to
             * the extended partition base, not to each EBR. */
            uint32_t base = s;
            uint32_t ebr = s;
            for (int g = 0; g < 16 && ebr; ++g) {
                uint8_t e[SECSZ];
                if (!g_rd(g_ctx, ebr, 1, e)) break;
                if (e[510] != 0x55 || e[511] != 0xAA) break;
                const uint8_t *l = e + 0x1BE;
                uint8_t lt = l[4];
                uint32_t lrel = (uint32_t)l[8] | ((uint32_t)l[9] << 8) |
                                ((uint32_t)l[10] << 16) | ((uint32_t)l[11] << 24);
                const uint8_t *nx = e + 0x1CE;
                uint8_t nt = nx[4];
                uint32_t nrel = (uint32_t)nx[8] | ((uint32_t)nx[9] << 8) |
                                ((uint32_t)nx[10] << 16) | ((uint32_t)nx[11] << 24);
                if (lt && lt != 0x05 && lt != 0x0F && lt != 0x85 &&
                    n < max && LooksFat32(base + lrel)) {
                    out[n++] = base + lrel;
                }
                ebr = (nt && nrel) ? base + nrel : 0;
            }
        } else if (n < max && LooksFat32(s)) {
            out[n++] = s;
        }
    }
    return n;
}

int Fat32_Mount(Fat32ReadSectors rd, void *ctx) {
    g_rd = rd;
    g_ctx = ctx;
    if (!rd) return 0;

    /* Superfloppy (VBR at LBA 0): no partition table at all. */
    if (LooksFat32(0)) {
        if (MountPartition(0)) return 1;
    }

    uint32_t cand[FAT32_MAX_CAND];
    int nc = CollectPartitions(cand, FAT32_MAX_CAND);
    if (nc == 0) return 0;

    /* Prefer the first partition that looks like a game/data volume; otherwise
     * fall back to the first usable FAT32 partition. */
    uint32_t first_ok = 0, chosen = 0;
    for (int i = 0; i < nc; ++i) {
        if (!MountPartition(cand[i])) continue;
        if (first_ok == 0) first_ok = cand[i];
        if (RootHasMarker()) { chosen = cand[i]; break; }
    }
    if (chosen == 0) chosen = first_ok;
    if (chosen == 0) return 0;
    if (!MountPartition(chosen)) return 0;   /* re-establish globals */
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

/* Assembles the raw 8.3 name of a directory entry (no case folding). */
static void BuildShortName(const uint8_t *e, char *out) {
    int n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; ++i) out[n++] = (char)e[i];
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; ++i) out[n++] = (char)e[i];
    }
    out[n] = 0;
}

long Fat32_ListDir(const char *path, Fat32DirFn fn, void *ctx) {
    /* `fn` may be null: then this just resolves the directory and returns its
     * entry count (>= 0) or -1 if it does not exist. */

    /* Resolve the directory path (empty / "/" = root). */
    uint32_t dir_clus = g_root_clus;
    int is_dir = 1;
    const char *p = path ? path : "";
    while (*p) {
        char comp[128];
        int n = 0;
        while (*p && *p != '/' && n < 127) comp[n++] = *p++;
        comp[n] = 0;
        if (*p == '/') ++p;
        if (comp[0] == 0) continue;
        if (!is_dir) return -1;
        uint32_t fc = 0, fs = 0; int fd = 0;
        if (!FindEntry(dir_clus, comp, &fc, &fs, &fd)) return -1;
        if (!fd) return -1;                    /* not a directory */
        dir_clus = fc;
    }

    char lfn[261];
    memset(lfn, 0, sizeof(lfn));
    long count = 0;
    uint32_t clus = dir_clus;
    uint32_t guard = 0;
    while (clus >= 2u && !IsEnd(clus) && guard++ < 65536u) {
        uint32_t lba = ClusterToLba(clus);
        for (uint32_t s = 0; s < g_spc; ++s) {
            if (!ReadSec(lba + s, g_sec)) return -1;
            for (uint32_t o = 0; o < SECSZ; o += 32u) {
                const uint8_t *e = g_sec + o;
                if (e[0] == 0x00) return count;             /* end of dir */
                if (e[0] == 0xE5) { lfn[0] = 0; continue; }  /* deleted */
                uint8_t attr = e[11];
                if (attr == ATTR_LFN) {
                    int seq = e[0] & 0x3F;
                    if (seq >= 1 && seq <= 20) LfnAppend13(lfn, seq, e);
                    continue;
                }
                if (e[0] == '.') { lfn[0] = 0; continue; }   /* '.' / '..' */
                if (attr & ATTR_VOLUME) { lfn[0] = 0; continue; }

                char name[288];
                if (lfn[0]) {
                    int i = 0;
                    for (; lfn[i] && i < 255; ++i) name[i] = lfn[i];
                    name[i] = 0;
                } else {
                    BuildShortName(e, name);
                }
                uint32_t size = (uint32_t)e[28] | ((uint32_t)e[29] << 8) |
                                ((uint32_t)e[30] << 16) | ((uint32_t)e[31] << 24);
                int is_this_dir = (attr & ATTR_DIR) ? 1 : 0;
                lfn[0] = 0;

                ++count;
                if (fn && fn(ctx, name, size, is_this_dir) == 0) return count;
            }
        }
        clus = FatNext(clus);
    }
    return count;
}
