/* fs.cpp: see fs.h.
 *
 * Layers the read/write FAT32 parser (xenon/source/fat32.c) on top of libXenon's
 * raw sector APIs. Two kinds of backing store are supported:
 *
 *   - USB mass storage (any number of devices, sector read + write)
 *   - The internal HDD over ATA (sector read + write)
 *
 * Both are exposed as "volumes" addressed by a small index. Volume 0 is the
 * default one used by the legacy single-volume calls, so older code is
 * unaffected. The stock Xbox 360 HDD is FATX, which we can neither read nor
 * write; it is detected and reported, never written to.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ppc/timebase.h>
#include <usb/usbmain.h> /* usb_init */

/* libXenon's bus + mass-storage entry points are not in a self-contained
 * header, so declare exactly what we use (same approach as xenon/source). */
extern "C" {
struct usbbus_s;
typedef struct usbbus_s usbbus_t;
void usb_poll(usbbus_t *bus);
void usb_daemon(usbbus_t *bus);
void usb_do_poll(void);
extern int       usb_buscnt;
extern usbbus_t *usb_buses[];

int USBStorage_Init(void);
int USBStorage_Get_Capacity(int device, unsigned int *sector_size);
int USBStorage_Read_Sectors(int device, unsigned int sector,
                            unsigned int numSectors, void *buffer);
int USBStorage_Write_Sectors(int device, unsigned int sector,
                             unsigned int numSectors, const void *buffer);

/* ATA (internal HDD): libXenon's public ata.h only exposes xenon_ata_init();
 * the sector read/write routines (ata_readsectors/ata_writesectors) are file
 * static and not exported, so the internal HDD is not sector-addressable from
 * the frontend. HDD volumes are therefore reported but never mounted. */
int xenon_ata_init(void);
}

#include "fs.h"
#include "fat32.h"

/* --- volumes ------------------------------------------------------------ */

struct Vol {
    int      used;
    Fat32   *fs;
    int      is_usb;   /* 1 = USB mass storage, 0 = ATA HDD        */
    int      dev;      /* USB device index (is_usb) or -1          */
    int      writable; /* backing store supports sector writes     */
    const char *type;  /* "USB" / "HDD"                            */
    const char *label; /* "usb" / "hdd"                            */
};

static Vol      g_vols[FS_MAX_VOLUMES];
static int      g_vol_count = 0;
static int      g_have_ata = 0;      /* HDD was probed at least once      */
static int      g_ata_type = 0;      /* last Fat32FsType seen on the HDD  */
static int      g_dev_count = 0;     /* USB mass-storage devices found    */
static int      g_ready = 0;
static char     g_roms[FS_PATH_MAX];
static char     g_scan_roots[FS_MAX_ROOTS][FS_PATH_MAX];
static int      g_scan_root_count = 0;
static char     g_devtype[16];
static char     g_diag[160];
static char     g_status[96];
static int      g_keys = -1;
static char     g_keys_status[64];
static unsigned char g_keybuf[65536];
static uint32_t g_keybuf_len = 0;

static void SetStatus(const char *s) {
    int i = 0;
    for (; s[i] && i < (int)sizeof(g_status) - 1; ++i) g_status[i] = s[i];
    g_status[i] = 0;
}

static void SetDiag(const char *s) {
    int i = 0;
    for (; s[i] && i < (int)sizeof(g_diag) - 1; ++i) g_diag[i] = s[i];
    g_diag[i] = 0;
}

/* Appends to the diagnostic buffer (so several volumes can report at once). */
static void AppendDiag(const char *s) {
    int n = 0; while (g_diag[n]) ++n;
    if (n && n < (int)sizeof(g_diag) - 2) g_diag[n++] = ';', g_diag[n++] = ' ';
    for (int i = 0; s[i] && n < (int)sizeof(g_diag) - 1; ++i) g_diag[n++] = s[i];
    g_diag[n] = 0;
}

static double Now(void) {
    return (double)mftb() / (double)PPC_TIMEBASE_FREQ;
}

/* Sector callbacks handed to fat32.c. `ctx` is always a Vol*. */
static int VolRead(void *ctx, uint32_t lba, uint32_t count, void *buf) {
    Vol *v = (Vol *)ctx;
    if (!v || !v->used) return 0;
    if (v->is_usb)
        return USBStorage_Read_Sectors(v->dev, (unsigned)lba, (unsigned)count, buf) ? 1 : 0;
    return 0; /* HDD: no public sector-read API in libXenon */
}

static int VolWrite(void *ctx, uint32_t lba, uint32_t count, const void *buf) {
    Vol *v = (Vol *)ctx;
    if (!v || !v->used || !v->writable) return 0;
    if (v->is_usb)
        return USBStorage_Write_Sectors(v->dev, (unsigned)lba, (unsigned)count, buf) ? 1 : 0;
    return 0; /* HDD: no public sector-write API in libXenon */
}

static void PollBuses(void) {
    for (int i = 0; i < usb_buscnt; ++i) {
        usbbus_t *b = usb_buses[i];
        if (b) { usb_poll(b); usb_daemon(b); }
    }
}

/* --- mount helpers ------------------------------------------------------ */

/* Mounts each USB mass-storage device as its own volume. Returns how many were
 * added. `tick` may be NULL. */
static void ProbeKeys(void);

static int MountUSB(int (*tick)(void *ctx, const char *status), void *ctx) {
    SetStatus("initialising USB");
    usb_init();

    double deadline = Now() + 6.0;
    int found = 0;
    int saw_device = 0;          /* a device reporting a positive capacity */
    int seen_cap = 0;
    unsigned int seen_ss = 0;
    g_dev_count = 0;
    while (Now() < deadline) {
        if (tick && !tick(ctx, "searching for a USB drive")) break;
        usb_do_poll();
        g_dev_count = 0;
        for (int d = 0; d < 8; ++d) {
            unsigned int ss = 0;
            int cap = USBStorage_Get_Capacity(d, &ss);
            if (cap > 0) { saw_device = 1; seen_cap = cap; seen_ss = ss; }
            /* A few bridges report the sector size late (0) but a valid
             * capacity; treat 0 as the universal 512 so "any USB" mounts. */
            if (cap > 0 && (ss == 512u || ss == 0u)) { ++g_dev_count; found = 1; }
        }
        if (found) break;
        double t0 = Now();
        while (Now() - t0 < 0.1) usb_do_poll();
    }

    if (!found) {
        /* Tell the two failure modes apart in the UI: nothing enumerated at all
         * (EHCI/DMA/layout problem) vs. a device that answered but with a sector
         * size we cannot mount. */
        char m[80];
        if (saw_device)
            snprintf(m, sizeof(m), "USB: device answered (cap=%d ss=%u)",
                     seen_cap, seen_ss);
        else
            snprintf(m, sizeof(m), "USB: nothing enumerated on EHCI/OHCI");
        AppendDiag(m);
        return 0;
    }

    int added = 0;
    for (int d = 0; d < 8 && g_vol_count < FS_MAX_VOLUMES; ++d) {
        unsigned int ss = 0;
        int cap = USBStorage_Get_Capacity(d, &ss);
        if (cap <= 0 || (ss != 512u && ss != 0u)) continue;

        Vol *v = &g_vols[g_vol_count];
        memset(v, 0, sizeof(*v));
        v->used = 1;
        v->is_usb = 1;
        v->dev = d;
        v->writable = 1; /* USBStorage_Write_Sectors is available */
        v->type = "USB";
        v->label = "usb";
        v->fs = Fat32_Open(VolRead, VolWrite, v);
        if (!v->fs) { v->used = 0; continue; }
        if (tick) tick(ctx, "mounting USB FAT32 volume");
        if (!Fat32_MountVolume(v->fs)) {
            Fat32FsType t = Fat32_Probe(VolRead, v);
            char m[64];
            snprintf(m, sizeof(m), "USB%d: not FAT32 (type %d)", d, (int)t);
            AppendDiag(m);
            Fat32_Close(v->fs);
            v->used = 0;
            continue;
        }
        ++g_vol_count;
        ++added;
    }
    return added;
}

/* Mounts the internal HDD if it holds a FAT32 volume. Returns 1 if added. */
static int MountHDD(int (*tick)(void *ctx, const char *status), void *ctx) {
    int first = !g_have_ata;
    g_have_ata = 1;
    if (tick && first) tick(ctx, "probing internal HDD");

    if (xenon_ata_init() != 0) {
        if (first) AppendDiag("HDD: no ATA device");
        return 0;
    }

    if (g_vol_count >= FS_MAX_VOLUMES) return 0;

    Vol *v = &g_vols[g_vol_count];
    memset(v, 0, sizeof(*v));
    v->used = 1;
    v->is_usb = 0;
    v->dev = -1;
    v->writable = 1;
    v->type = "HDD";
    v->label = "hdd";

    Fat32FsType t = Fat32_Probe(VolRead, v);
    g_ata_type = (int)t;
    if (t != FAT32_FS_FAT32) {
        const char *nm = (t == FAT32_FS_FATX) ? "FATX (unsupported)"
                       : (t == FAT32_FS_FAT16) ? "FAT16 (unsupported)"
                       : "unknown";
        char m[80];
        snprintf(m, sizeof(m), "HDD: %s, read-only use impossible", nm);
        AppendDiag(m);
        v->used = 0;
        return 0;
    }

    v->fs = Fat32_Open(VolRead, VolWrite, v);
    if (!v->fs) { v->used = 0; return 0; }
    if (tick) tick(ctx, "mounting HDD FAT32 volume");
    if (!Fat32_MountVolume(v->fs)) {
        AppendDiag("HDD: FAT32 mount failed");
        Fat32_Close(v->fs);
        v->used = 0;
        return 0;
    }
    ++g_vol_count;
    return 1;
}

/* Discovers the conventional scan roots on volume 0 and fills g_roms. The walk
 * in app.cpp always starts at the volume root, so these are only used for the
 * informational "Folder:" line and the keys probe. */
static int DirExistsOn(int vol, const char *path) {
    return Fs_ListDirOn(vol, path, 0, 0) != -1;
}

static void ResolveRomsDir(void) {
    static const char *cands[] = {
        "roms", "ROMs", "ROMS", "roms/switch", "games", "GAMES", "Games",
        "switch", "SWITCH", "eden/roms", "Eden/roms", "EDEN/ROMS", "roms/switch"
    };
    g_roms[0] = 0;
    g_scan_root_count = 0;
    for (unsigned i = 0; i < sizeof(cands) / sizeof(cands[0]) && g_scan_root_count < FS_MAX_ROOTS; ++i) {
        int n = 0;
        const char *c = cands[i];
        for (; c[n] && n < FS_PATH_MAX - 1; ++n) g_scan_roots[g_scan_root_count][n] = c[n];
        g_scan_roots[g_scan_root_count][n] = 0;
        ++g_scan_root_count;
        if (DirExistsOn(0, c) && !g_roms[0])
            memcpy(g_roms, g_scan_roots[g_scan_root_count - 1], FS_PATH_MAX);
    }
    if (g_scan_root_count < FS_MAX_ROOTS) {
        g_scan_roots[g_scan_root_count][0] = 0;
        ++g_scan_root_count;
    }
}

int Fs_MountAll(int (*tick)(void *ctx, const char *status), void *ctx) {
    g_ready = 0;
    g_vol_count = 0;
    g_dev_count = 0;
    g_diag[0] = 0;
    memset(g_vols, 0, sizeof(g_vols));

    if (g_have_ata) {
        /* ATA already initialised by an earlier call: re-mount without the
         * potential first-init delay. */
    }

    int usb = MountUSB(tick, ctx);
    /* Only probe the internal ATA/HDD when no USB volume mounted. xenon_ata_init()
     * pokes ATA/SATA registers, and on a stock 360 the internal drive is FATX
     * (unsupported) anyway, so probing it after a good USB mount only adds noise
     * and side effects. If USB turns up nothing, the HDD is still tried so a
     * FAT32 internal drive can be used. */
    int hdd = (usb <= 0) ? MountHDD(tick, ctx) : 0;

    if (g_vol_count <= 0) {
        g_ready = 0;
        if (g_dev_count <= 0)      AppendDiag("no USB mass-storage device");
        else                       AppendDiag("USB device present but no FAT32 volume");
        if (!g_diag[0])            SetDiag("no storage");
        /* Fold the reason into the status so it is visible in the UI. */
        char buf[96];
        snprintf(buf, sizeof(buf), "no FAT32 volume (%s)", g_diag);
        SetStatus(buf);
        return 0;
    }

    g_ready = 1;
    ResolveRomsDir();

    /* The default volume is USB if any USB volume mounted, else the HDD. */
    int def = 0;
    for (int i = 0; i < g_vol_count; ++i)
        if (g_vols[i].is_usb) { def = i; break; }
    if (def != 0) { Vol t = g_vols[0]; g_vols[0] = g_vols[def]; g_vols[def] = t; }
    snprintf(g_devtype, sizeof(g_devtype), "%s", g_vols[0].type);

    char buf[96];
    if (usb && hdd)      snprintf(buf, sizeof(buf), "USB + HDD ready (%d vol)", g_vol_count);
    else if (hdd)        snprintf(buf, sizeof(buf), "HDD FAT32 ready");
    else                 snprintf(buf, sizeof(buf), "USB FAT32 ready (%d dev%s)",
                                  g_dev_count, g_dev_count == 1 ? "" : "s");
    SetStatus(buf);
    ProbeKeys();
    return g_vol_count;
}

int Fs_Mount(int (*tick)(void *ctx, const char *status), void *ctx) {
    return Fs_MountAll(tick, ctx) > 0 ? 1 : 0;
}

int Fs_EnsureMounted(void) {
    if (g_ready) return 1;
    Fs_MountAll(0, 0);
    return g_ready;
}

int Fs_Ready(void) { return g_ready; }
const char *Fs_RomsDir(void) { return g_roms; }
const char *Fs_Status(void) { return g_status[0] ? g_status : "not mounted"; }
int Fs_DeviceCount(void) { return g_dev_count; }
uint64_t Fs_VolumeSize(void) { return g_ready ? Fat32_TotalBytes(g_vols[0].fs) : 0; }
int Fs_KeyCount(void) { return g_keys; }
const char *Fs_KeysStatus(void) {
    return g_keys_status[0] ? g_keys_status : "prod.keys: not checked";
}

/* --- volume accessors --------------------------------------------------- */

int Fs_VolumeCount(void) { return g_vol_count; }

const char *Fs_VolumeType(int vol) {
    if (vol < 0 || vol >= g_vol_count) return "";
    return g_vols[vol].type;
}

const char *Fs_VolumeLabel(int vol) {
    if (vol < 0 || vol >= g_vol_count) return "";
    return g_vols[vol].label;
}

int Fs_VolumeWritable(int vol) {
    if (vol < 0 || vol >= g_vol_count) return 0;
    return g_vols[vol].writable && Fat32_IsWritable(g_vols[vol].fs);
}

uint64_t Fs_VolumeTotal(int vol) {
    if (vol < 0 || vol >= g_vol_count) return 0;
    return Fat32_TotalBytes(g_vols[vol].fs);
}

uint64_t Fs_VolumeFree(int vol) {
    if (vol < 0 || vol >= g_vol_count) return 0;
    return Fat32_FreeBytes(g_vols[vol].fs);
}

static int CiEqStr(const char *a, const char *b) {
    for (;;) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return 0;
        if (!ca) return 1;
        ++a; ++b;
    }
}

int Fs_VolumeByType(const char *type) {
    if (!type) return -1;
    for (int i = 0; i < g_vol_count; ++i)
        if (CiEqStr(g_vols[i].type, type)) return i;
    return -1;
}

int Fs_VolumeByLabel(const char *label) {
    if (!label) return -1;
    for (int i = 0; i < g_vol_count; ++i)
        if (CiEqStr(g_vols[i].label, label)) return i;
    return -1;
}

const char *Fs_VolumesSummary(void) {
    static char s[160];
    int off = 0;
    s[0] = 0;
    if (!g_ready) { snprintf(s, sizeof(s), "(none)"); return s; }
    for (int i = 0; i < g_vol_count && off < (int)sizeof(s) - 24; ++i) {
        uint64_t gb10 = Fs_VolumeTotal(i) * 10u / (1024ull * 1024ull * 1024ull);
        int w = snprintf(s + off, sizeof(s) - off, "%s%s:%llu.%lluGB%s",
                         i ? " " : "", g_vols[i].type,
                         (unsigned long long)(gb10 / 10),
                         (unsigned long long)(gb10 % 10),
                         g_vols[i].writable ? "" : "r/o");
        if (w <= 0) break;
        off += w;
    }
    return s;
}

/* --- per-volume operations --------------------------------------------- */

long Fs_ListDirOn(int vol, const char *path, FsDirFn fn, void *ctx) {
    if (vol < 0 || vol >= g_vol_count) return -1;
    if (!path) path = "";
    return Fat32V_ListDir(g_vols[vol].fs, path, fn ? (Fat32DirFn)fn : 0, ctx);
}

long Fs_ReadFileOn(int vol, const char *path, void *buf, uint32_t max) {
    if (vol < 0 || vol >= g_vol_count || !path) return -1;
    return Fat32V_ReadFile(g_vols[vol].fs, path, buf, max);
}

/* --- streaming range reader (large containers) ------------------------- */

void *Fs_OpenReader(int vol, const char *path, uint64_t *out_size) {
    if (vol < 0 || vol >= g_vol_count || !path) return 0;
    uint32_t sz = 0;
    Fat32Reader *r = Fat32Reader_Open(g_vols[vol].fs, path, &sz);
    if (!r) return 0;
    if (out_size) *out_size = sz;
    return r;
}

long Fs_ReaderRead(void *h, void *buf, uint32_t len) {
    return h ? Fat32Reader_Read((Fat32Reader *)h, buf, len) : -1;
}

int Fs_ReaderSeek(void *h, uint32_t off) {
    return h ? Fat32Reader_Seek((Fat32Reader *)h, off) : 0;
}

void Fs_ReaderClose(void *h) {
    if (h) Fat32Reader_Close((Fat32Reader *)h);
}

const unsigned char *Fs_KeyBuf(uint32_t *out_len) {
    if (out_len) *out_len = g_keybuf_len;
    return g_keybuf_len ? g_keybuf : 0;
}

int Fs_ExistsOn(int vol, const char *path, int *is_dir) {
    if (vol < 0 || vol >= g_vol_count || !path) return 0;
    uint32_t sz = 0;
    int dir = 0;
    int r = Fat32V_Stat(g_vols[vol].fs, path, &sz, &dir);
    if (r && is_dir) *is_dir = dir;
    return r;
}

int Fs_MkdirOn(int vol, const char *path) {
    if (vol < 0 || vol >= g_vol_count || !path) return 0;
    return Fat32V_Mkdir(g_vols[vol].fs, path);
}

int Fs_UnlinkOn(int vol, const char *path) {
    if (vol < 0 || vol >= g_vol_count || !path || !path[0]) return 0;
    if (!Fs_VolumeWritable(vol)) return 0;
    return Fat32V_Unlink(g_vols[vol].fs, path);
}

int Fs_MkdirsOn(int vol, const char *path) {
    if (vol < 0 || vol >= g_vol_count || !path || !path[0]) return 0;
    char tmp[FS_PATH_MAX];
    int n = 0;
    for (; path[n] && n < FS_PATH_MAX - 1; ++n) tmp[n] = path[n];
    tmp[n] = 0;

    /* Strip a leading slash: paths are volume-relative. */
    char *p = tmp;
    while (*p == '/') ++p;

    for (char *q = p; ; ++q) {
        if (*q == '/' || *q == 0) {
            char save = *q;
            *q = 0;
            if (p[0]) {
                if (!Fat32V_Exists(g_vols[vol].fs, p) && !Fat32V_Mkdir(g_vols[vol].fs, p))
                    return 0;
            }
            *q = save;
            if (save == 0) break;
        }
    }
    return 1;
}

/* --- file copy (streaming, verified) ------------------------------------ */

#define FS_COPY_CHUNK 65536
static unsigned char g_copybuf[FS_COPY_CHUNK];

int Fs_CopyFileOn(int dst_vol, const char *dst_path,
                  int src_vol, const char *src_path,
                  FsCopyProgress cb, void *cbctx, uint64_t *out_bytes) {
    if (dst_vol < 0 || dst_vol >= g_vol_count || !dst_path) return 0;
    if (src_vol < 0 || src_vol >= g_vol_count || !src_path) return 0;
    if (!Fs_VolumeWritable(dst_vol)) return 0;

    uint32_t size = 0;
    Fat32Reader *r = Fat32Reader_Open(g_vols[src_vol].fs, src_path, &size);
    if (!r) return 0;

    Fat32File *f = Fat32File_Create(g_vols[dst_vol].fs, dst_path);
    if (!f) { Fat32Reader_Close(r); return 0; }

    uint64_t done = 0;
    int ok = 1;
    for (;;) {
        long got = Fat32Reader_Read(r, g_copybuf, FS_COPY_CHUNK);
        if (got < 0) { ok = 0; break; }
        if (got == 0) break;
        if (!Fat32File_Write(f, g_copybuf, (uint32_t)got)) { ok = 0; break; }
        done += (uint64_t)got;
        if (cb && !cb(cbctx, done, size)) { ok = 0; break; }
    }
    Fat32Reader_Close(r);

    if (!ok) { Fat32File_Abort(f); return 0; }
    if (!Fat32File_Finish(f)) return 0;

    /* Verify the copied byte count. */
    uint32_t wsz = 0;
    if (!Fat32V_Stat(g_vols[dst_vol].fs, dst_path, &wsz, 0) || wsz != size) return 0;
    if (out_bytes) *out_bytes = (uint64_t)size;
    return 1;
}

/* --- legacy single-volume wrappers -------------------------------------- */

long Fs_ListDir(const char *path, FsDirFn fn, void *ctx) {
    return Fs_ListDirOn(0, path, fn, ctx);
}

long Fs_ReadFile(const char *path, void *buf, uint32_t max) {
    return Fs_ReadFileOn(0, path, buf, max);
}

/* Sets the keys status string. */
static void SetKeysStatus(const char *s) {
    int i = 0;
    for (; s[i] && i < (int)sizeof(g_keys_status) - 1; ++i) g_keys_status[i] = s[i];
    g_keys_status[i] = 0;
}

/* Counts "name = value" lines (ignoring blanks, comments and lines with an
 * empty key). */
static int CountKeyEntries(const unsigned char *b, long n) {
    int count = 0;
    long i = 0;
    while (i < n) {
        long ls = i;
        while (i < n && b[i] != '\n') ++i;
        long le = i;
        if (i < n) ++i; /* consume newline */

        long a = ls, z = le;
        while (a < z && (b[a] == ' ' || b[a] == '\t' || b[a] == '\r')) ++a;
        while (z > a && (b[z - 1] == ' ' || b[z - 1] == '\t' || b[z - 1] == '\r')) --z;
        if (a >= z) continue;
        if (b[a] == '#' || b[a] == ';') continue;

        long k = a;
        while (k < z && b[k] != '=' && b[k] != ' ' && b[k] != '\t') ++k;
        int keyLen = (int)(k - a);
        int hasEq = 0;
        for (long j = a; j < z; ++j) if (b[j] == '=') { hasEq = 1; break; }
        if (hasEq && keyLen > 0) ++count;
    }
    return count;
}

/* Short summary of the volume root (first few names) for on-screen
 * diagnostics. fat32.c keeps its state per volume, so directory listings are
 * never nested: each listing is collected fully before the next begins. */
#define FS_ROOT_NAMES 8
static char g_root_summary[160];
const char *Fs_RootSummary(void) { return g_root_summary; }

static char LowerCh(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int CiEq(const char *a, const char *b) {
    for (;;) {
        if (LowerCh(*a) != LowerCh(*b)) return 0;
        if (!*a) return 1;
        ++a; ++b;
    }
}
static int CiHasKey(const char *s) {           /* contains "key" (case-folded) */
    for (; *s; ++s)
        if (LowerCh(s[0])=='k' && LowerCh(s[1])=='e' && LowerCh(s[2])=='y') return 1;
    return 0;
}
static int CiEndsKeys(const char *s) {         /* ends with ".keys" */
    int n = 0; while (s[n]) ++n;
    if (n < 5) return 0;
    const char *e = s + n - 5;
    return e[0]=='.' && LowerCh(e[1])=='k' && LowerCh(e[2])=='e' &&
           LowerCh(e[3])=='y' && LowerCh(e[4])=='s';
}
static int CiIsProdKeys(const char *s) {
    return CiEq(s, "prod.keys");
}

struct NameListCtx { char names[FS_ROOT_NAMES][80]; int is_dir[FS_ROOT_NAMES]; int n; };
static int NameListCb(void *ctx, const char *name, uint32_t size, int is_dir) {
    (void)size;
    struct NameListCtx *c = (struct NameListCtx *)ctx;
    if (c->n >= FS_ROOT_NAMES) return 0;
    int i = 0; for (; name[i] && i < 79; ++i) c->names[c->n][i] = name[i];
    c->names[c->n][i] = 0;
    c->is_dir[c->n] = is_dir;
    ++c->n;
    return 1;
}

/* Lists `dir` and copies the name of the first `*.keys` file into `out`. */
static int FindKeysInDir(const char *dir, char *out, int outsz) {
    struct NameListCtx c; c.n = 0;
    if (Fs_ListDirOn(0, dir, NameListCb, &c) < 0) return 0;
    for (int i = 0; i < c.n; ++i) {
        if (!c.is_dir[i] && (CiIsProdKeys(c.names[i]) || CiEndsKeys(c.names[i]))) {
            int k = 0; for (; c.names[i][k] && k < outsz - 1; ++k) out[k] = c.names[i][k];
            out[k] = 0;
            return 1;
        }
    }
    return 0;
}

/* Looks for a prod.keys file and reports how many entries it has.
 * Informational only: this build does not use keys. */
static void ProbeKeys(void) {
    static const char *cands[] = {
        "keys/prod.keys", "KEYS/PROD.KEYS", "Keys/prod.keys", "keys/PROD.KEYS",
        "prod.keys", "PROD.KEYS", "yuzu/keys/prod.keys",
        "eden/keys/prod.keys", "Eden/keys/prod.keys", "EDEN/KEYS/PROD.KEYS"
    };
    g_keys = -1;
    char diag[192]; diag[0] = 0;

    /* 1) Fast path: known relative locations. */
    for (unsigned i = 0; i < sizeof(cands)/sizeof(cands[0]); ++i) {
        long n = Fs_ReadFileOn(0, cands[i], g_keybuf, (uint32_t)sizeof(g_keybuf));
        if (n > 0) {
            g_keys = CountKeyEntries(g_keybuf, n); g_keybuf_len = (uint32_t)n;
            char buf[80];
            snprintf(buf, sizeof(buf), "prod.keys: %d keys (%s)", g_keys, cands[i]);
            SetKeysStatus(buf);
            snprintf(diag, sizeof(diag), "found prod.keys at %s (%ld bytes)", cands[i], n);
            AppendDiag(diag);
            return;
        }
    }

    /* 2) Enumerate the volume root once. */
    struct NameListCtx root; root.n = 0;
    Fs_ListDirOn(0, "", NameListCb, &root);

    /* Build the on-screen root summary. */
    {
        int off = 0;
        for (int i = 0; i < root.n && off < (int)sizeof(g_root_summary) - 12; ++i) {
            int w = snprintf(g_root_summary + off, sizeof(g_root_summary) - off,
                             "%s%s%s", root.names[i], root.is_dir[i] ? "/" : "",
                             (i + 1 < root.n) ? " " : "");
            if (w <= 0) break;
            off += w;
        }
        if (off == 0) snprintf(g_root_summary, sizeof(g_root_summary), "(root empty)");
    }

    /* 3) Look for `*.keys` directly in the root. */
    for (int i = 0; i < root.n; ++i) {
        if (!root.is_dir[i] && (CiIsProdKeys(root.names[i]) || CiEndsKeys(root.names[i]))) {
            long n = Fs_ReadFileOn(0, root.names[i], g_keybuf, (uint32_t)sizeof(g_keybuf));
            if (n > 0) {
                g_keys = CountKeyEntries(g_keybuf, n); g_keybuf_len = (uint32_t)n;
                snprintf(diag, sizeof(diag), "found %s (%ld bytes)", root.names[i], n);
                AppendDiag(diag);
                goto done;
            }
        }
    }

    /* 4) Any top-level directory whose name mentions "key". */
    for (int i = 0; i < root.n; ++i) {
        if (!root.is_dir[i] || !CiHasKey(root.names[i])) continue;
        char file[80];
        if (FindKeysInDir(root.names[i], file, sizeof(file))) {
            char path[FS_PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", root.names[i], file);
            long n = Fs_ReadFileOn(0, path, g_keybuf, (uint32_t)sizeof(g_keybuf));
            if (n > 0) {
                g_keys = CountKeyEntries(g_keybuf, n); g_keybuf_len = (uint32_t)n;
                snprintf(diag, sizeof(diag), "found %s (%ld bytes)", path, n);
                AppendDiag(diag);
                goto done;
            }
        }
    }

    SetKeysStatus("prod.keys: not found");
    snprintf(diag, sizeof(diag), "prod.keys not found; root: %s", g_root_summary);
    AppendDiag(diag);
    return;

done:
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "prod.keys: %d keys", g_keys);
        SetKeysStatus(buf);
    }
}

int Fs_GetScanRoots(const char **roots, int max_roots) {
    int n = 0;
    for (int i = 0; i < g_scan_root_count && i < max_roots; ++i) {
        roots[i] = g_scan_roots[i];
        ++n;
    }
    return n;
}

const char *Fs_DeviceType(void) {
    return g_devtype[0] ? g_devtype : "";
}

const char *Fs_LastDiag(void) {
    return g_diag[0] ? g_diag : "";
}
