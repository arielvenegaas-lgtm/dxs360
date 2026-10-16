/* fat32.c: see fat32.h.
 *
 * Instance based so several volumes (USB + HDD) can be mounted at once, with
 * optional write support (mkdir / create / streaming write / unlink) used by
 * the frontend's "copy game to HDD" feature.
 */
#include <string.h>
#include <stdlib.h>

#include "fat32.h"

#define SECSZ        512u
#define FAT32_BAD    0x0FFFFFF7u
#define FAT32_EOC    0x0FFFFFFFu
#define ATTR_LFN     0x0Fu
#define ATTR_DIR     0x10u
#define ATTR_VOLUME  0x08u

/* Upper bound on a directory we will load into RAM to edit it. */
#define DIR_BUF_MAX  (4u * 1024u * 1024u)

struct Fat32 {
    Fat32ReadSectors  rd;
    Fat32WriteSectors wr;
    void     *ctx;

    uint32_t part_start;
    uint32_t spc;         /* sectors per cluster */
    uint32_t fat_start;   /* first FAT LBA        */
    uint32_t fat_sz;      /* sectors per FAT      */
    uint32_t num_fats;
    uint32_t data_start;  /* first data LBA       */
    uint32_t root_clus;
    uint32_t fsinfo_sec;  /* FSInfo sector LBA (0 if none) */
    uint32_t clus_cnt;    /* data clusters, 0 if unknown */
    uint32_t next_free;   /* allocation hint cluster */
    uint32_t free_clus;   /* cached free cluster count */

    int mounted;
    int writable;

    uint8_t sec[SECSZ];   /* scratch sector */
};

/* --- little-endian helpers --------------------------------------------- */
static uint16_t Rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t Rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void Wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void Wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* --- raw I/O ------------------------------------------------------------ */
static int ReadSec(Fat32 *fs, uint32_t lba, void *buf) {
    return fs->rd(fs->ctx, lba, 1u, buf);
}
static int WriteSec(Fat32 *fs, uint32_t lba, const void *buf) {
    if (!fs->wr) return 0;
    return fs->wr(fs->ctx, lba, 1u, buf);
}

static uint32_t ClusterBytes(const Fat32 *fs) { return fs->spc * SECSZ; }

static int IsEnd(const Fat32 *fs, uint32_t clus) {
    if (clus < 2u) return 1;
    if (clus >= FAT32_BAD) return 1;
    if (fs->clus_cnt != 0 && clus >= 2u + fs->clus_cnt) return 1;
    return 0;
}

static uint32_t ClusterToLba(const Fat32 *fs, uint32_t n) {
    return fs->data_start + (n - 2u) * fs->spc;
}

static uint32_t FatGet(Fat32 *fs, uint32_t clus) {
    uint32_t off = clus * 4u;
    uint32_t lba = fs->fat_start + off / SECSZ;
    uint32_t o   = off % SECSZ;
    if (!ReadSec(fs, lba, fs->sec)) return FAT32_BAD;
    return Rd32(fs->sec + o) & 0x0FFFFFFFu;
}

/* Unknown → treat as chain terminator (already in FatGet via FAT32_BAD). */
static uint32_t FatNext(Fat32 *fs, uint32_t clus) { return FatGet(fs, clus); }

static int FatSet(Fat32 *fs, uint32_t clus, uint32_t val) {
    uint32_t off = clus * 4u;
    uint32_t o   = off % SECSZ;
    for (uint32_t f = 0; f < fs->num_fats; ++f) {
        uint32_t lba = fs->fat_start + f * fs->fat_sz + off / SECSZ;
        if (!ReadSec(fs, lba, fs->sec)) return 0;
        uint32_t cur = Rd32(fs->sec + o);
        cur = (cur & 0xF0000000u) | (val & 0x0FFFFFFFu);
        Wr32(fs->sec + o, cur);
        if (!WriteSec(fs, lba, fs->sec)) return 0;
    }
    return 1;
}

/* --- geometry ----------------------------------------------------------- */
static int LooksFat32(Fat32 *fs, uint32_t lba) {
    if (!ReadSec(fs, lba, fs->sec)) return 0;
    const uint8_t *b = fs->sec;
    uint32_t bps = Rd16(b + 11);
    uint32_t spc = b[13];
    uint32_t root_ent = Rd16(b + 17);
    uint32_t fsz16 = Rd16(b + 22);
    if (bps != SECSZ || spc == 0) return 0;
    if (root_ent != 0 || fsz16 != 0) return 0;      /* FAT12/16 */
    if (b[510] != 0x55 || b[511] != 0xAA) return 0;
    return 1;
}

static void ParseBpb(Fat32 *fs) {
    const uint8_t *b = fs->sec;
    uint32_t res = Rd16(b + 14);
    fs->fat_sz = Rd32(b + 36);
    fs->num_fats = b[16];
    fs->spc = b[13];
    fs->fat_start = res;
    fs->data_start = res + fs->num_fats * fs->fat_sz;
    fs->root_clus = Rd32(b + 44);
    fs->fsinfo_sec = Rd16(b + 48);
    /* Total sectors is TotSec16 when non-zero, else TotSec32 (FAT32 spec). */
    uint32_t tot = Rd16(b + 19);
    if (tot == 0) tot = Rd32(b + 32);
    if (tot > fs->data_start)
        fs->clus_cnt = (tot - fs->data_start) / (fs->spc ? fs->spc : 1u);
    else
        fs->clus_cnt = 0;
}

static int MountPartition(Fat32 *fs, uint32_t start) {
    if (!ReadSec(fs, start, fs->sec)) return 0;
    ParseBpb(fs);
    fs->fat_start += start;
    fs->data_start += start;
    if (fs->fsinfo_sec) fs->fsinfo_sec += start;
    fs->part_start = start;
    if (fs->spc == 0 || fs->num_fats == 0 || fs->fat_sz == 0) return 0;
    if (fs->fat_start == start || fs->root_clus < 2u) return 0;
    return 1;
}

static int RootHasMarker(Fat32 *fs) {
    static const char *m[] = { "roms", "ROMs", "keys", "games", "switch" };
    for (unsigned i = 0; i < sizeof(m) / sizeof(m[0]); ++i)
        if (Fat32V_ListDir(fs, m[i], 0, 0) != -1) return 1;
    return 0;
}

#define FAT32_MAX_CAND 16

static int CollectPartitions(Fat32 *fs, uint32_t *out, int max) {
    int n = 0;
    uint8_t tbl[SECSZ];
    if (!fs->rd(fs->ctx, 0, 1, tbl)) return 0;
    if (tbl[510] != 0x55 || tbl[511] != 0xAA) return 0;
    for (int i = 0; i < 4; ++i) {
        const uint8_t *pe = tbl + 0x1BE + i * 16;
        uint8_t type = pe[4];
        if (type == 0) continue;
        uint32_t s = Rd32(pe + 8);
        if (s == 0) continue;
        if (type == 0x05 || type == 0x0F || type == 0x85) {
            uint32_t base = s, ebr = s;
            for (int g = 0; g < 16 && ebr; ++g) {
                uint8_t e[SECSZ];
                if (!fs->rd(fs->ctx, ebr, 1, e)) break;
                if (e[510] != 0x55 || e[511] != 0xAA) break;
                const uint8_t *l = e + 0x1BE;
                uint8_t lt = l[4];
                uint32_t lrel = Rd32(l + 8);
                const uint8_t *nx = e + 0x1CE;
                uint8_t nt = nx[4];
                uint32_t nrel = Rd32(nx + 8);
                if (lt && lt != 0x05 && lt != 0x0F && lt != 0x85 &&
                    n < max && LooksFat32(fs, base + lrel))
                    out[n++] = base + lrel;
                ebr = (nt && nrel) ? base + nrel : 0;
            }
        } else if (n < max && LooksFat32(fs, s)) {
            out[n++] = s;
        }
    }
    return n;
}

Fat32 *Fat32_Open(Fat32ReadSectors rd, Fat32WriteSectors wr, void *ctx) {
    if (!rd) return 0;
    Fat32 *fs = (Fat32 *)calloc(1, sizeof(Fat32));
    if (!fs) return 0;
    fs->rd = rd;
    fs->wr = wr;
    fs->ctx = ctx;
    return fs;
}

void Fat32_Close(Fat32 *fs) {
    if (fs) free(fs);
}

static uint32_t CountFreeClusters(Fat32 *fs) {
    if (fs->clus_cnt == 0) return 0;
    uint32_t free_clus = 0;
    uint32_t end = 2u + fs->clus_cnt;
    for (uint32_t cl = 2; cl < end; ) {
        uint32_t off = cl * 4u;
        uint32_t lba = fs->fat_start + off / SECSZ;
        uint32_t o = off % SECSZ;
        uint32_t per = (SECSZ - o) / 4u;
        if (per == 0) { ++cl; continue; }
        if (!ReadSec(fs, lba, fs->sec)) break;
        for (uint32_t i = 0; i < per && cl < end; ++i, ++cl) {
            uint32_t v = Rd32(fs->sec + o + i * 4u) & 0x0FFFFFFFu;
            if (v == 0) ++free_clus;
        }
    }
    return free_clus;
}

int Fat32_MountVolume(Fat32 *fs) {
    if (!fs) return 0;
    fs->mounted = 0;
    fs->next_free = 2;
    fs->free_clus = 0;

    int have = 0;
    if (LooksFat32(fs, 0)) {
        have = MountPartition(fs, 0);
    }
    if (!have) {
        uint32_t cand[FAT32_MAX_CAND];
        int nc = CollectPartitions(fs, cand, FAT32_MAX_CAND);
        if (nc == 0) return 0;
        uint32_t first_ok = 0, chosen = 0;
        for (int i = 0; i < nc; ++i) {
            if (!MountPartition(fs, cand[i])) continue;
            /* Mark "mounted" so RootHasMarker's directory reads are allowed. */
            fs->mounted = 1;
            if (first_ok == 0) first_ok = cand[i];
            if (RootHasMarker(fs)) { chosen = cand[i]; break; }
        }
        if (chosen == 0) chosen = first_ok;
        if (chosen == 0) { fs->mounted = 0; return 0; }
        if (!MountPartition(fs, chosen)) { fs->mounted = 0; return 0; }
    }
    fs->mounted = 1;
    fs->writable = (fs->wr != 0);
    fs->free_clus = CountFreeClusters(fs);
    return 1;
}

int      Fat32_IsWritable(const Fat32 *fs) { return fs && fs->writable; }
uint64_t Fat32_TotalBytes(const Fat32 *fs) {
    return fs ? (uint64_t)fs->clus_cnt * ClusterBytes(fs) : 0;
}
uint64_t Fat32_FreeBytes(const Fat32 *fs) {
    return fs ? (uint64_t)fs->free_clus * ClusterBytes(fs) : 0;
}

/* --- probing ------------------------------------------------------------ */
static int LooksFatx(Fat32 *fs, uint32_t lba) {
    if (!ReadSec(fs, lba, fs->sec)) return 0;
    return fs->sec[0] == 'F' && fs->sec[1] == 'A' &&
           fs->sec[2] == 'T' && fs->sec[3] == 'X';
}

Fat32FsType Fat32_Probe(Fat32ReadSectors rd, void *ctx) {
    Fat32 *fs = Fat32_Open(rd, 0, ctx);
    if (!fs) return FAT32_FS_UNKNOWN;
    Fat32FsType t = FAT32_FS_UNKNOWN;

    uint8_t sec0[SECSZ];
    if (!rd(ctx, 0, 1, sec0)) { Fat32_Close(fs); return FAT32_FS_NONE; }

    if (LooksFatx(fs, 0)) { t = FAT32_FS_FATX; goto done; }
    if (LooksFat32(fs, 0)) { t = FAT32_FS_FAT32; goto done; }

    if (sec0[510] == 0x55 && sec0[511] == 0xAA) {
        for (int i = 0; i < 4; ++i) {
            const uint8_t *pe = sec0 + 0x1BE + i * 16;
            uint8_t type = pe[4];
            if (type == 0) continue;
            if (type == 0x05 || type == 0x0F || type == 0x85) continue;
            uint32_t s = Rd32(pe + 8);
            if (s == 0) continue;
            if (LooksFatx(fs, s)) { t = FAT32_FS_FATX; goto done; }
            if (LooksFat32(fs, s)) { t = FAT32_FS_FAT32; goto done; }
            if (type == 0x04 || type == 0x06 || type == 0x0E) { t = FAT32_FS_FAT16; goto done; }
        }
        /* Extended partitions: best-effort check of the first logical volume. */
        for (int i = 0; i < 4; ++i) {
            const uint8_t *pe = sec0 + 0x1BE + i * 16;
            uint8_t type = pe[4];
            if (type != 0x05 && type != 0x0F && type != 0x85) continue;
            uint32_t base = Rd32(pe + 8);
            uint8_t e[SECSZ];
            if (!rd(ctx, base, 1, e)) continue;
            const uint8_t *l = e + 0x1BE;
            uint32_t lrel = Rd32(l + 8);
            if (lrel && LooksFat32(fs, base + lrel)) { t = FAT32_FS_FAT32; goto done; }
            if (lrel && LooksFatx(fs, base + lrel))  { t = FAT32_FS_FATX; goto done; }
        }
    }
done:
    Fat32_Close(fs);
    return t;
}

/* --- long filename helpers --------------------------------------------- */
static int MatchShort(const uint8_t *e, const char *want) {
    char name[13];
    int n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; ++i) {
        char c = (char)e[i];
        name[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    if (e[8] != ' ') {
        name[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; ++i) {
            char c = (char)e[i];
            name[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        }
    }
    name[n] = 0;
    const char *p = want;
    for (int i = 0; i < n; ++i) {
        char wc = *p;
        if (wc >= 'a' && wc <= 'z') wc = (char)(wc - 32);
        if (!*p || wc != name[i]) return 0;
        ++p;
    }
    return *p == 0;
}

static int MatchLong(const char *lfn, const char *want) {
    for (;;) {
        char a = *lfn, b = *want;
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 0;
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

typedef struct {
    uint32_t clus;
    uint32_t size;
    uint32_t lba;   /* sector holding the 8.3 entry */
    uint32_t off;   /* offset of the 8.3 entry in that sector */
    int is_dir;
} DirEnt;

/* Finds `comp` in directory at `dir_clus`. Returns 1 and fills `out`. */
static int FindEntry(Fat32 *fs, uint32_t dir_clus, const char *comp, DirEnt *out) {
    char lfn[261];
    memset(lfn, 0, sizeof(lfn));
    uint32_t clus = dir_clus;
    uint32_t guard = 0;
    while (clus >= 2u && !IsEnd(fs, clus) && guard++ < 65536u) {
        uint32_t lba = ClusterToLba(fs, clus);
        for (uint32_t s = 0; s < fs->spc; ++s) {
            if (!ReadSec(fs, lba + s, fs->sec)) return 0;
            for (uint32_t o = 0; o < SECSZ; o += 32u) {
                const uint8_t *e = fs->sec + o;
                if (e[0] == 0x00) return 0;
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
                    uint32_t hi = Rd16(e + 20);
                    uint32_t lo = Rd16(e + 26);
                    out->clus = (hi << 16) | lo;
                    out->size = Rd32(e + 28);
                    out->is_dir = (attr & ATTR_DIR) ? 1 : 0;
                    out->lba = lba + s;
                    out->off = o;
                    return 1;
                }
                lfn[0] = 0;
            }
        }
        clus = FatNext(fs, clus);
    }
    return 0;
}

/* Resolves a directory path to its start cluster. Returns 1 on success. */
static int ResolveDir(Fat32 *fs, const char *path, uint32_t *out_clus) {
    uint32_t clus = fs->root_clus;
    const char *p = path ? path : "";
    while (*p == '/') ++p;
    while (*p) {
        char comp[128];
        int n = 0;
        while (*p && *p != '/' && n < 127) comp[n++] = *p++;
        comp[n] = 0;
        if (*p == '/') ++p;
        if (comp[0] == 0) continue;
        DirEnt e;
        if (!FindEntry(fs, clus, comp, &e)) return 0;
        if (!e.is_dir) return 0;
        clus = e.clus;
    }
    *out_clus = clus;
    return 1;
}

/* Splits "a/b/name" into parent path and final component. Returns 1. */
static int SplitPath(const char *path, char *parent, int psz, char *name, int nsz) {
    const char *p = path ? path : "";
    while (*p == '/') ++p;
    const char *last = 0;
    for (const char *q = p; *q; ++q) if (*q == '/') last = q;
    const char *np = last ? last + 1 : p;
    int n = 0;
    for (; np[n] && n < nsz - 1; ++n) name[n] = np[n];
    name[n] = 0;
    if (n == 0) return 0;
    int pn = last ? (int)(last - p) : 0;
    if (pn > psz - 1) pn = psz - 1;
    memcpy(parent, p, (size_t)pn);
    parent[pn] = 0;
    return 1;
}

long Fat32V_ListDir(Fat32 *fs, const char *path, Fat32DirFn fn, void *ctx) {
    if (!fs || !fs->mounted) return -1;
    uint32_t dir_clus;
    if (!ResolveDir(fs, path, &dir_clus)) return -1;

    char lfn[261];
    memset(lfn, 0, sizeof(lfn));
    long count = 0;
    uint32_t clus = dir_clus;
    uint32_t guard = 0;
    while (clus >= 2u && !IsEnd(fs, clus) && guard++ < 65536u) {
        uint32_t lba = ClusterToLba(fs, clus);
        for (uint32_t s = 0; s < fs->spc; ++s) {
            if (!ReadSec(fs, lba + s, fs->sec)) return -1;
            for (uint32_t o = 0; o < SECSZ; o += 32u) {
                const uint8_t *e = fs->sec + o;
                if (e[0] == 0x00) return count;
                if (e[0] == 0xE5) { lfn[0] = 0; continue; }
                uint8_t attr = e[11];
                if (attr == ATTR_LFN) {
                    int seq = e[0] & 0x3F;
                    if (seq >= 1 && seq <= 20) LfnAppend13(lfn, seq, e);
                    continue;
                }
                if (e[0] == '.') { lfn[0] = 0; continue; }
                if (attr & ATTR_VOLUME) { lfn[0] = 0; continue; }

                char name[288];
                if (lfn[0]) {
                    int i = 0;
                    for (; lfn[i] && i < 255; ++i) name[i] = lfn[i];
                    name[i] = 0;
                } else {
                    int k = 0;
                    for (int i = 0; i < 8 && e[i] != ' '; ++i) name[k++] = (char)e[i];
                    if (e[8] != ' ') {
                        name[k++] = '.';
                        for (int i = 8; i < 11 && e[i] != ' '; ++i) name[k++] = (char)e[i];
                    }
                    name[k] = 0;
                }
                uint32_t size = Rd32(e + 28);
                int is_dir = (attr & ATTR_DIR) ? 1 : 0;
                lfn[0] = 0;

                ++count;
                if (fn && fn(ctx, name, size, is_dir) == 0) return count;
            }
        }
        clus = FatNext(fs, clus);
    }
    return count;
}

long Fat32V_ReadFile(Fat32 *fs, const char *path, void *buf, uint32_t maxlen) {
    if (!fs || !fs->mounted || !path || !buf) return -1;
    char parent[256], name[256];
    if (!SplitPath(path, parent, sizeof(parent), name, sizeof(name))) return -1;
    uint32_t dir_clus;
    if (!ResolveDir(fs, parent, &dir_clus)) return -1;
    DirEnt e;
    if (!FindEntry(fs, dir_clus, name, &e) || e.is_dir) return -1;

    uint32_t left = e.size; if (left > maxlen) left = maxlen;
    uint32_t got = 0, c = e.clus, guard = 0;
    while (c >= 2u && !IsEnd(fs, c) && got < left && guard++ < 65536u) {
        uint32_t lba = ClusterToLba(fs, c);
        for (uint32_t s = 0; s < fs->spc && got < left; ++s) {
            if (!ReadSec(fs, lba + s, fs->sec)) return -1;
            uint32_t chunk = left - got; if (chunk > SECSZ) chunk = SECSZ;
            memcpy((uint8_t *)buf + got, fs->sec, chunk);
            got += chunk;
        }
        c = FatNext(fs, c);
    }
    return (long)got;
}

int Fat32V_Stat(Fat32 *fs, const char *path, uint32_t *out_size, int *out_isdir) {
    if (!fs || !fs->mounted) return 0;
    const char *p = path ? path : "";
    while (*p == '/') ++p;
    if (*p == 0) {
        if (out_size) *out_size = 0;
        if (out_isdir) *out_isdir = 1;
        return 1;
    }
    char parent[256], name[256];
    if (!SplitPath(path, parent, sizeof(parent), name, sizeof(name))) return 0;
    uint32_t dir_clus;
    if (!ResolveDir(fs, parent, &dir_clus)) return 0;
    DirEnt e;
    if (!FindEntry(fs, dir_clus, name, &e)) return 0;
    if (out_size) *out_size = e.size;
    if (out_isdir) *out_isdir = e.is_dir;
    return 1;
}

int Fat32V_Exists(Fat32 *fs, const char *path) {
    return Fat32V_Stat(fs, path, 0, 0);
}

/* --- writing ------------------------------------------------------------ */

static uint32_t AllocCluster(Fat32 *fs) {
    if (!fs->writable || fs->clus_cnt == 0) return 0;
    if (fs->free_clus == 0) return 0;
    uint32_t total = fs->clus_cnt;
    uint32_t c = fs->next_free;
    if (c < 2u || c >= 2u + total) c = 2u;
    for (uint32_t i = 0; i < total; ++i) {
        if (FatNext(fs, c) == 0) {
            if (!FatSet(fs, c, FAT32_EOC)) return 0;
            fs->next_free = (c + 1u >= 2u + total) ? 2u : c + 1u;
            if (fs->free_clus) --fs->free_clus;
            return c;
        }
        if (++c >= 2u + total) c = 2u;
    }
    return 0;
}

static void FreeChain(Fat32 *fs, uint32_t clus) {
    uint32_t guard = 0;
    while (clus >= 2u && !IsEnd(fs, clus) && guard++ < 0x7FFFFF00u) {
        uint32_t next = FatNext(fs, clus);
        FatSet(fs, clus, 0);
        if (fs->writable) ++fs->free_clus;
        if (next >= FAT32_BAD) break;
        clus = next;
    }
}

static int ZeroCluster(Fat32 *fs, uint32_t clus) {
    memset(fs->sec, 0, SECSZ);
    uint32_t lba = ClusterToLba(fs, clus);
    for (uint32_t s = 0; s < fs->spc; ++s)
        if (!WriteSec(fs, lba + s, fs->sec)) return 0;
    return 1;
}

/* Keeps the FSInfo sector's free-cluster / next-free hints in step with our
 * allocation bookkeeping so fsck and Windows don't see a stale summary. Only
 * touches the sector when its signatures are intact. */
#define FSINFO_LEAD  0x41615252u
#define FSINFO_STRUC 0x61417272u
static void WriteFsInfo(Fat32 *fs) {
    if (!fs->writable || fs->fsinfo_sec == 0) return;
    if (fs->clus_cnt && fs->free_clus > fs->clus_cnt) return;
    if (!ReadSec(fs, fs->fsinfo_sec, fs->sec)) return;
    if (Rd32(fs->sec) != FSINFO_LEAD || Rd32(fs->sec + 484) != FSINFO_STRUC)
        return;
    Wr32(fs->sec + 488, fs->free_clus);
    Wr32(fs->sec + 492, fs->next_free);
    WriteSec(fs, fs->fsinfo_sec, fs->sec);
}

/* --- directory editing (load whole dir into RAM) ------------------------ */

typedef struct { uint8_t *buf; uint32_t len; uint32_t cap; uint32_t last_clus; } DirBuf;

static int DirLoad(Fat32 *fs, uint32_t dir_clus, DirBuf *d) {
    d->buf = 0; d->len = 0; d->cap = 0; d->last_clus = dir_clus;
    uint32_t cbytes = ClusterBytes(fs);
    uint32_t clus = dir_clus, guard = 0;
    while (clus >= 2u && !IsEnd(fs, clus) && guard++ < 0x7FFFFF00u) {
        if (d->len + cbytes > DIR_BUF_MAX) return 0;
        uint8_t *nb = (uint8_t *)realloc(d->buf, d->len + cbytes);
        if (!nb) { free(d->buf); d->buf = 0; return 0; }
        d->buf = nb;
        uint32_t lba = ClusterToLba(fs, clus);
        for (uint32_t s = 0; s < fs->spc; ++s) {
            if (!ReadSec(fs, lba + s, d->buf + d->len + s * SECSZ)) {
                free(d->buf); d->buf = 0; return 0;
            }
        }
        d->len += cbytes;
        d->last_clus = clus;
        clus = FatNext(fs, clus);
    }
    return d->buf != 0;
}

static void DirFree(DirBuf *d) { if (d->buf) free(d->buf); d->buf = 0; }

static int DirCommit(Fat32 *fs, uint32_t dir_clus, DirBuf *d) {
    uint32_t clus = dir_clus, off = 0, guard = 0;
    while (clus >= 2u && !IsEnd(fs, clus) && off < d->len && guard++ < 0x7FFFFF00u) {
        uint32_t lba = ClusterToLba(fs, clus);
        for (uint32_t s = 0; s < fs->spc && off < d->len; ++s, off += SECSZ)
            if (!WriteSec(fs, lba + s, d->buf + off)) return 0;
        clus = FatNext(fs, clus);
    }
    return 1;
}

/* Appends one cluster to the directory chain and extends the RAM buffer. */
static int DirExtend(Fat32 *fs, DirBuf *d) {
    uint32_t cbytes = ClusterBytes(fs);
    if (d->len + cbytes > DIR_BUF_MAX) return 0;
    uint32_t nc = AllocCluster(fs);
    if (!nc) return 0;
    if (!FatSet(fs, d->last_clus, nc)) { FatSet(fs, nc, 0); return 0; }
    if (!ZeroCluster(fs, nc)) return 0;
    uint8_t *nb = (uint8_t *)realloc(d->buf, d->len + cbytes);
    if (!nb) return 0;
    d->buf = nb;
    memset(d->buf + d->len, 0, cbytes);
    d->len += cbytes;
    d->last_clus = nc;
    return 1;
}

static uint8_t ShortChecksum(const uint8_t short11[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; ++i)
        sum = (uint8_t)(((sum & 1) ? 0x80u : 0u) + (sum >> 1) + short11[i]);
    return sum;
}

static int CharShortOk(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c=='$'||c=='%'||c=='\''||c=='-'||c=='_'||c=='@'||c=='~'||
           c=='`'||c=='!'||c=='('||c==')'||c=='{'||c=='}'||c=='^'||
           c=='#'||c=='&';
}

static char ShortUp(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* True if `short11` already appears in the in-memory directory. */
static int ShortUsed(const DirBuf *d, const uint8_t short11[11]) {
    for (uint32_t o = 0; o + 32 <= d->len; o += 32) {
        const uint8_t *e = d->buf + o;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) continue;
        if ((e[11] & ATTR_LFN) == ATTR_LFN) continue;
        if (e[11] & ATTR_VOLUME) continue;
        int same = 1;
        for (int i = 0; i < 11; ++i) if (e[i] != short11[i]) { same = 0; break; }
        if (same) return 1;
    }
    return 0;
}

/* Builds an 11-byte 8.3 short name for `longname`, unique within `d`. */
static void MakeShortName(const DirBuf *d, const char *longname, uint8_t out11[11]) {
    /* Split at the last '.' */
    const char *dot = 0;
    for (const char *q = longname; *q; ++q) if (*q == '.') dot = q;
    char base[128], ext[16];
    int bn = 0, en = 0;
    const char *bp = longname;
    const char *ep = dot ? dot + 1 : 0;
    int blen = dot ? (int)(dot - longname) : (int)strlen(longname);
    for (int i = 0; i < blen && bn < (int)sizeof(base) - 1; ++i) base[bn++] = bp[i];
    base[bn] = 0;
    if (ep) { for (; *ep && en < (int)sizeof(ext) - 1; ++ep) ext[en++] = *ep; }
    ext[en] = 0;

    int lossy = 0;
    char sb[9], se[4];
    int sbn = 0, sen = 0;
    for (int i = 0; i < bn; ++i) {
        char c = ShortUp(base[i]);
        if (!CharShortOk(c)) { c = '_'; lossy = 1; }
        if (base[i] >= 'a' && base[i] <= 'z') lossy = 1;
        if (sbn < 8) sb[sbn++] = c; else lossy = 1;
    }
    if (sbn == 0) { sb[sbn++] = '_'; }
    for (int i = 0; i < en; ++i) {
        char c = ShortUp(ext[i]);
        if (!CharShortOk(c)) { c = '_'; lossy = 1; }
        if (ext[i] >= 'a' && ext[i] <= 'z') lossy = 1;
        if (sen < 3) se[sen++] = c; else lossy = 1;
    }

    uint8_t cand[11];
    memset(cand, ' ', 11);
    if (!lossy) {
        memcpy(cand, sb, (size_t)sbn);
        memcpy(cand + 8, se, (size_t)sen);
        if (!ShortUsed(d, cand)) { memcpy(out11, cand, 11); return; }
        lossy = 1;
    }

    /* Generate BASE~N. Keep up to 6 base chars to leave room for ~N. */
    int keep = sbn > 6 ? 6 : sbn;
    for (int num = 1; num < 1000000; ++num) {
        char tail[8];
        int tn = 0;
        tail[tn++] = '~';
        char digits[8]; int dn = 0;
        int t = num;
        while (t) { digits[dn++] = (char)('0' + t % 10); t /= 10; }
        for (int i = dn - 1; i >= 0; --i) tail[tn++] = digits[i];
        if (keep + tn > 8) { keep = 8 - tn; if (keep < 1) keep = 1; }
        memset(cand, ' ', 11);
        memcpy(cand, sb, (size_t)keep);
        memcpy(cand + keep, tail, (size_t)tn);
        memcpy(cand + 8, se, (size_t)sen);
        if (!ShortUsed(d, cand)) { memcpy(out11, cand, 11); return; }
    }
    memcpy(out11, cand, 11);
}

/* Builds the LFN entries (physically stored high-seq first). Returns count, or
 * 0 if a plain 8.3 name is enough. */
static int BuildLfn(const char *longname, const uint8_t short11[11], uint8_t *out, int maxent) {
    int len = 0; while (longname[len]) ++len;
    /* Decide whether an LFN is needed: name isn't the same as the 8.3 form. */
    char shortdisp[14];
    int sn = 0;
    for (int i = 0; i < 8 && short11[i] != ' '; ++i) shortdisp[sn++] = (char)short11[i];
    if (short11[8] != ' ') {
        shortdisp[sn++] = '.';
        for (int i = 8; i < 11 && short11[i] != ' '; ++i) shortdisp[sn++] = (char)short11[i];
    }
    shortdisp[sn] = 0;
    if (MatchLong(shortdisp, longname) && strchr(longname, '.') == strrchr(longname, '.') &&
        !strchr(longname, '~')) {
        /* Looks like a genuine 8.3 name already. */
        int need = 0;
        for (int i = 0; i < len; ++i) {
            char c = longname[i];
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                CharShortOk(ShortUp(c))) continue;
            need = 1; break;
        }
        if (!need && len <= 12) return 0;
    }

    int units = (len + 12) / 13;
    if (units < 1) units = 1;
    if (units > maxent) return -1;
    uint8_t chk = ShortChecksum(short11);
    uint8_t *dst = out;
    for (int seq = units; seq >= 1; --seq) {
        memset(dst, 0, 32);
        dst[0] = (uint8_t)seq;
        if (seq == units) dst[0] |= 0x40;
        dst[11] = ATTR_LFN;
        dst[12] = 0;
        dst[13] = chk;
        dst[26] = 0; dst[27] = 0;
        static const int off[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
        for (int i = 0; i < 13; ++i) {
            int idx = (seq - 1) * 13 + i;
            uint16_t c;
            if (idx < len) c = (uint8_t)longname[idx];
            else c = (idx == len) ? 0x0000 : 0xFFFF;
            dst[off[i]] = (uint8_t)(c & 0xFF);
            dst[off[i] + 1] = (uint8_t)(c >> 8);
        }
        dst += 32;
    }
    return units;
}

/* Finds `need` consecutive free slots (0x00 or 0xE5) in the dir buffer.
 * Returns the byte offset of the first one, or -1 (caller may extend). */
static long DirFindFree(const DirBuf *d, int need) {
    long run = -1;
    int have = 0;
    for (uint32_t o = 0; o + 32 <= d->len; o += 32) {
        const uint8_t *e = d->buf + o;
        int free_slot = (e[0] == 0x00 || e[0] == 0xE5);
        if (free_slot) {
            if (have == 0) run = (long)o;
            if (++have >= need) return run;
        } else {
            have = 0; run = -1;
        }
    }
    return -1;
}

/* Ensures a terminator (0x00) exists at `after` (byte offset) or extends. */
static int DirEnsureTerminator(Fat32 *fs, DirBuf *d, uint32_t after) {
    if (after + 32 <= d->len) return 1;   /* next slot is already >= 0 */
    return DirExtend(fs, d);
}

/* Removes the 8.3 entry at slot `idx` plus its preceding contiguous LFN run. */
static void DirMarkDeleted(DirBuf *d, uint32_t idx) {
    d->buf[idx] = 0xE5;
    uint32_t j = idx;
    while (j >= 32) {
        j -= 32;
        if ((d->buf[j + 11] & ATTR_LFN) == ATTR_LFN && d->buf[j] != 0xE5) {
            d->buf[j] = 0xE5;
            if (d->buf[j + 0] & 0x40) break;   /* last unit */
        } else break;
    }
}

/* Finds the 8.3 entry for `name` (LFN- or 8.3-comparison) in the RAM dir,
 * returning its byte offset or -1. */
static long DirFindEntry(const DirBuf *d, const char *name) {
    char lfn[261];
    memset(lfn, 0, sizeof(lfn));
    for (uint32_t o = 0; o + 32 <= d->len; o += 32) {
        const uint8_t *e = d->buf + o;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) { lfn[0] = 0; continue; }
        uint8_t attr = e[11];
        if (attr == ATTR_LFN) {
            int seq = e[0] & 0x3F;
            if (seq >= 1 && seq <= 20) LfnAppend13(lfn, seq, e);
            continue;
        }
        if (attr & ATTR_VOLUME) { lfn[0] = 0; continue; }
        if (e[0] == '.') { lfn[0] = 0; continue; }
        int ok = 0;
        if (lfn[0]) ok = MatchLong(lfn, name);
        if (!ok && MatchShort(e, name)) ok = 1;
        if (ok) return (long)o;
        lfn[0] = 0;
    }
    return -1;
}

static int Fat32V_MkdirImpl(Fat32 *fs, const char *path) {
    if (!fs || !fs->writable) return 0;
    char parent[256], name[256];
    if (!SplitPath(path, parent, sizeof(parent), name, sizeof(name))) return 0;
    uint32_t parent_clus;
    if (!ResolveDir(fs, parent, &parent_clus)) return 0;

    DirEnt ex;
    if (FindEntry(fs, parent_clus, name, &ex)) return ex.is_dir ? 1 : 0;

    DirBuf d;
    if (!DirLoad(fs, parent_clus, &d)) return 0;

    uint8_t short11[11];
    MakeShortName(&d, name, short11);
    uint8_t lfn[21 * 32];
    int lunits = BuildLfn(name, short11, lfn, 20);
    if (lunits < 0) { DirFree(&d); return 0; }
    int need = lunits + 1;

    long at = DirFindFree(&d, need);
    if (at < 0) {
        if (!DirExtend(fs, &d)) { DirFree(&d); return 0; }
        at = DirFindFree(&d, need);
    }
    if (at < 0) { DirFree(&d); return 0; }

    uint32_t newc = AllocCluster(fs);
    if (!newc) { DirFree(&d); return 0; }
    if (!ZeroCluster(fs, newc)) { FatSet(fs, newc, 0); DirFree(&d); return 0; }

    /* "." and ".." are the first two 32-byte entries of the cluster and live in
     * its first sector. WriteSec() emits a full 512-byte sector, so build both
     * entries in the (zeroed) sector buffer. */
    {
        uint8_t *s = fs->sec;
        uint32_t pcl = (parent_clus == fs->root_clus) ? 0u : parent_clus;
        memset(s, 0, SECSZ);
        memset(s, ' ', 11); s[0] = '.'; s[11] = ATTR_DIR;
        Wr16(s + 20, (uint16_t)(newc >> 16)); Wr16(s + 26, (uint16_t)newc);
        memset(s + 32, ' ', 11); s[32] = '.'; s[33] = '.'; s[43] = ATTR_DIR;
        Wr16(s + 52, (uint16_t)(pcl >> 16)); Wr16(s + 58, (uint16_t)pcl);
        if (!WriteSec(fs, ClusterToLba(fs, newc), s)) { FatSet(fs, newc, 0); DirFree(&d); return 0; }
    }

    uint8_t *p = d.buf + at;
    if (lunits > 0) { memcpy(p, lfn, (size_t)lunits * 32); p += lunits * 32; }
    memset(p, 0, 32);
    memcpy(p, short11, 11);
    p[11] = ATTR_DIR;
    Wr16(p + 20, (uint16_t)(newc >> 16));
    Wr16(p + 26, (uint16_t)newc);
    Wr32(p + 28, 0);

    if (!DirEnsureTerminator(fs, &d, (uint32_t)at + (uint32_t)need * 32)) {
        FatSet(fs, newc, 0); DirFree(&d); return 0;
    }
    int ok = DirCommit(fs, parent_clus, &d);
    if (!ok) FatSet(fs, newc, 0);
    DirFree(&d);
    if (ok) WriteFsInfo(fs);
    return ok;
}

int Fat32V_Mkdir(Fat32 *fs, const char *path) {
    return Fat32V_MkdirImpl(fs, path);
}

int Fat32V_Unlink(Fat32 *fs, const char *path) {
    if (!fs || !fs->writable) return 0;
    char parent[256], name[256];
    if (!SplitPath(path, parent, sizeof(parent), name, sizeof(name))) return 0;
    uint32_t parent_clus;
    if (!ResolveDir(fs, parent, &parent_clus)) return 0;
    DirEnt e;
    if (!FindEntry(fs, parent_clus, name, &e) || e.is_dir) return 0;

    FreeChain(fs, e.clus);

    DirBuf d;
    if (!DirLoad(fs, parent_clus, &d)) return 0;
    long idx = DirFindEntry(&d, name);
    if (idx < 0) { DirFree(&d); return 0; }
    DirMarkDeleted(&d, (uint32_t)idx);
    int ok = DirCommit(fs, parent_clus, &d);
    DirFree(&d);
    if (ok) WriteFsInfo(fs);
    return ok;
}

/* --- streaming writer --------------------------------------------------- */
struct Fat32File {
    Fat32   *fs;
    uint32_t parent_clus;
    char     name[256];
    uint32_t first_clus;
    uint32_t cur;
    uint32_t clus_fill;
    uint32_t size;
    uint8_t  sec[SECSZ];
    uint32_t sec_n;
    int      ok;
};

static int FileFlushSec(Fat32File *f) {
    Fat32 *fs = f->fs;
    uint32_t cbytes = ClusterBytes(fs);
    if (f->cur == 0) {
        uint32_t c = AllocCluster(fs);
        if (!c) { f->ok = 0; return 0; }
        f->first_clus = f->cur = c;
        f->clus_fill = 0;
    } else if (f->clus_fill >= cbytes) {
        uint32_t c = AllocCluster(fs);
        if (!c) { f->ok = 0; return 0; }
        if (!FatSet(fs, f->cur, c)) { f->ok = 0; return 0; }
        f->cur = c;
        f->clus_fill = 0;
    }
    uint32_t lba = ClusterToLba(fs, f->cur) + f->clus_fill / SECSZ;
    if (!WriteSec(fs, lba, f->sec)) { f->ok = 0; return 0; }
    f->clus_fill += SECSZ;
    f->sec_n = 0;
    return 1;
}

Fat32File *Fat32File_Create(Fat32 *fs, const char *path) {
    if (!fs || !fs->writable) return 0;
    char parent[256], name[256];
    if (!SplitPath(path, parent, sizeof(parent), name, sizeof(name))) return 0;
    uint32_t parent_clus;
    if (!ResolveDir(fs, parent, &parent_clus)) return 0;

    /* If it already exists, remove it first (truncate semantics). */
    DirEnt ex;
    if (FindEntry(fs, parent_clus, name, &ex)) {
        if (ex.is_dir) return 0;
        Fat32V_Unlink(fs, path);
    }

    DirBuf d;
    if (!DirLoad(fs, parent_clus, &d)) return 0;
    uint8_t short11[11];
    MakeShortName(&d, name, short11);
    uint8_t lfn[21 * 32];
    int lunits = BuildLfn(name, short11, lfn, 20);
    if (lunits < 0) { DirFree(&d); return 0; }
    int need = lunits + 1;
    long at = DirFindFree(&d, need);
    if (at < 0) {
        if (!DirExtend(fs, &d)) { DirFree(&d); return 0; }
        at = DirFindFree(&d, need);
    }
    if (at < 0) { DirFree(&d); return 0; }

    uint8_t *p = d.buf + at;
    if (lunits > 0) { memcpy(p, lfn, (size_t)lunits * 32); p += lunits * 32; }
    memset(p, 0, 32);
    memcpy(p, short11, 11);
    p[11] = 0;                 /* regular file */
    Wr32(p + 28, 0);           /* size 0 for now */

    if (!DirEnsureTerminator(fs, &d, (uint32_t)at + (uint32_t)need * 32)) { DirFree(&d); return 0; }
    if (!DirCommit(fs, parent_clus, &d)) { DirFree(&d); return 0; }
    DirFree(&d);

    Fat32File *f = (Fat32File *)calloc(1, sizeof(Fat32File));
    if (!f) return 0;
    f->fs = fs;
    f->parent_clus = parent_clus;
    int i = 0; for (; name[i] && i < 255; ++i) f->name[i] = name[i]; f->name[i] = 0;
    f->ok = 1;
    return f;
}

int Fat32File_Write(Fat32File *f, const void *data, uint32_t len) {
    if (!f || !f->ok) return 0;
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        uint32_t space = SECSZ - f->sec_n;
        uint32_t take = len < space ? len : space;
        memcpy(f->sec + f->sec_n, p, take);
        f->sec_n += take; p += take; len -= take; f->size += take;
        if (f->sec_n == SECSZ) if (!FileFlushSec(f)) return 0;
    }
    return 1;
}

/* Removes the entry named `name` from directory `parent_clus` (LFN aware). */
static void RemoveEntryByName(Fat32 *fs, uint32_t parent_clus, const char *name) {
    DirBuf d;
    if (!DirLoad(fs, parent_clus, &d)) return;
    long idx = DirFindEntry(&d, name);
    if (idx < 0) { DirFree(&d); return; }
    DirMarkDeleted(&d, (uint32_t)idx);
    DirCommit(fs, parent_clus, &d);
    DirFree(&d);
}

/* Updates the file's directory entry with its first cluster and size. */
static int FileUpdateEntry(Fat32File *f) {
    Fat32 *fs = f->fs;
    DirEnt e;
    if (!FindEntry(fs, f->parent_clus, f->name, &e)) return 0;
    if (!ReadSec(fs, e.lba, fs->sec)) return 0;
    uint8_t *p = fs->sec + e.off;
    Wr16(p + 20, (uint16_t)(f->first_clus >> 16));
    Wr16(p + 26, (uint16_t)(f->first_clus & 0xFFFF));
    Wr32(p + 28, f->size);
    return WriteSec(fs, e.lba, fs->sec);
}

int Fat32File_Finish(Fat32File *f) {
    if (!f) return 0;
    int ok = f->ok;
    if (ok && f->sec_n > 0) {
        memset(f->sec + f->sec_n, 0, SECSZ - f->sec_n);
        f->sec_n = SECSZ;
        if (!FileFlushSec(f)) ok = 0;
    }
    if (ok) ok = FileUpdateEntry(f);
    if (!ok) {
        /* roll back: free clusters and remove the (partial) entry */
        Fat32 *fs = f->fs;
        if (f->first_clus) { FreeChain(fs, f->first_clus); f->first_clus = 0; }
        RemoveEntryByName(fs, f->parent_clus, f->name);
        WriteFsInfo(fs);
    } else {
        WriteFsInfo(f->fs);
    }
    free(f);
    return ok;
}

void Fat32File_Abort(Fat32File *f) {
    if (!f) return;
    Fat32 *fs = f->fs;
    if (f->first_clus) { FreeChain(fs, f->first_clus); f->first_clus = 0; }
    RemoveEntryByName(fs, f->parent_clus, f->name);
    WriteFsInfo(fs);
    free(f);
}

int Fat32V_WriteFile(Fat32 *fs, const char *path, const void *data, uint32_t len) {
    Fat32File *f = Fat32File_Create(fs, path);
    if (!f) return 0;
    if (len && !Fat32File_Write(f, data, len)) { Fat32File_Abort(f); return 0; }
    return Fat32File_Finish(f);
}

/* --- streaming reader --------------------------------------------------- */
struct Fat32Reader {
    Fat32   *fs;
    uint32_t size;
    uint32_t pos;
    uint32_t clus;
    uint32_t first_clus;
    uint32_t clus_off;
    uint32_t cache_lba;
    int      cache_ok;
    uint8_t  sec[SECSZ];
};

Fat32Reader *Fat32Reader_Open(Fat32 *fs, const char *path, uint32_t *out_size) {
    if (!fs || !fs->mounted || !path) return 0;
    char parent[256], name[256];
    if (!SplitPath(path, parent, sizeof(parent), name, sizeof(name))) return 0;
    uint32_t dir_clus;
    if (!ResolveDir(fs, parent, &dir_clus)) return 0;
    DirEnt e;
    if (!FindEntry(fs, dir_clus, name, &e) || e.is_dir) return 0;

    Fat32Reader *r = (Fat32Reader *)calloc(1, sizeof(Fat32Reader));
    if (!r) return 0;
    r->fs = fs;
    r->size = e.size;
    r->pos = 0;
    r->clus = e.clus;
    r->first_clus = e.clus;
    r->clus_off = 0;
    r->cache_ok = 0;
    if (out_size) *out_size = e.size;
    return r;
}

long Fat32Reader_Read(Fat32Reader *r, void *buf, uint32_t len) {
    if (!r) return -1;
    Fat32 *fs = r->fs;
    uint32_t cbytes = ClusterBytes(fs);
    uint8_t *out = (uint8_t *)buf;
    uint32_t got = 0;
    while (got < len && r->pos < r->size) {
        if (r->clus < 2u || IsEnd(fs, r->clus)) break;
        if (r->clus_off >= cbytes) {
            r->clus = FatNext(fs, r->clus);
            r->clus_off = 0;
            continue;
        }
        uint32_t sec_idx = r->clus_off / SECSZ;
        uint32_t sec_off = r->clus_off % SECSZ;
        uint32_t lba = ClusterToLba(fs, r->clus) + sec_idx;
        if (!r->cache_ok || r->cache_lba != lba) {
            if (!ReadSec(fs, lba, r->sec)) return got ? (long)got : -1;
            r->cache_lba = lba;
            r->cache_ok = 1;
        }
        uint32_t want = len - got;
        if (want > r->size - r->pos) want = r->size - r->pos;
        uint32_t avail = SECSZ - sec_off;
        if (want > avail) want = avail;
        memcpy(out + got, r->sec + sec_off, want);
        got += want;
        r->pos += want;
        r->clus_off += want;
    }
    return (long)got;
}

void Fat32Reader_Close(Fat32Reader *r) { if (r) free(r); }

/* Repositions the reader to absolute byte offset `off`. Walks the FAT chain
 * (following only cluster links, not data sectors) from the file's first
 * cluster to the cluster holding `off`. Returns 1 on success, 0 if `off` is
 * past EOF or the chain is shorter than expected. Needed to read a single NCA
 * section out of a large NSP without streaming the whole container. */
int Fat32Reader_Seek(Fat32Reader *r, uint32_t off) {
    if (!r) return 0;
    if (off > r->size) return 0;
    Fat32 *fs = r->fs;
    uint32_t cbytes = ClusterBytes(fs);
    /* Re-start from the first cluster and walk forward cluster by cluster. */
    uint32_t clus = r->first_clus;
    uint32_t idx = off / cbytes;
    uint32_t i;
    for (i = 0; i < idx; ++i) {
        if (clus < 2u || IsEnd(fs, clus)) return 0;
        clus = FatNext(fs, clus);
    }
    if (idx > 0 && (clus < 2u || IsEnd(fs, clus))) return 0;
    r->clus = clus;
    r->clus_off = off % cbytes;
    r->pos = off;
    r->cache_ok = 0;
    return 1;
}

/* --- legacy global API -------------------------------------------------- */
static Fat32 *g_default;

int Fat32_Mount(Fat32ReadSectors rd, void *ctx) {
    if (g_default) { Fat32_Close(g_default); g_default = 0; }
    g_default = Fat32_Open(rd, 0, ctx);
    if (!g_default) return 0;
    if (!Fat32_MountVolume(g_default)) { Fat32_Close(g_default); g_default = 0; return 0; }
    return 1;
}

long Fat32_ReadFile(const char *path, void *buf, uint32_t maxlen) {
    if (!g_default) return -1;
    return Fat32V_ReadFile(g_default, path, buf, maxlen);
}

long Fat32_ListDir(const char *path, Fat32DirFn fn, void *ctx) {
    if (!g_default) return -1;
    return Fat32V_ListDir(g_default, path, fn, ctx);
}
