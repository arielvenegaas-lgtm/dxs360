/* fs.cpp: see fs.h. */
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

int xenon_ata_init(void);
}

#include "fs.h"
#include "fat32.h"

static int  g_dev  = -1;
static int  g_dev_count = 0;
static uint64_t g_vol_size = 0;
static int  g_ready = 0;
static char g_roms[FS_PATH_MAX];
static char g_scan_roots[FS_MAX_ROOTS][FS_PATH_MAX];
static int  g_scan_root_count = 0;
static char g_devtype[16];
static char g_diag[128];
static char g_status[96];
static int  g_keys = -1;
static char g_keys_status[64];
static unsigned char g_keybuf[65536];

static int ReadSectors(void *ctx, uint32_t lba, uint32_t count, void *buf) {
    (void)ctx;
    if (g_dev < 0) return 0;
    return USBStorage_Read_Sectors(g_dev, (unsigned)lba, (unsigned)count, buf) ? 1 : 0;
}

static void PollBuses(void) {
    for (int i = 0; i < usb_buscnt; ++i) {
        usbbus_t *b = usb_buses[i];
        if (b) { usb_poll(b); usb_daemon(b); }
    }
}

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

static double Now(void) {
    return (double)mftb() / (double)PPC_TIMEBASE_FREQ;
}

int Fs_EnsureMounted(void) {
    if (g_ready) return 1;
    Fs_Mount(0, 0);
    return g_ready;
}

int Fs_Ready(void) { return g_ready; }
const char *Fs_RomsDir(void) { return g_roms; }
const char *Fs_Status(void) { return g_status[0] ? g_status : "not mounted"; }
int Fs_DeviceCount(void) { return g_dev_count; }
uint64_t Fs_VolumeSize(void) { return g_vol_size; }
int Fs_KeyCount(void) { return g_keys; }
const char *Fs_KeysStatus(void) {
    return g_keys_status[0] ? g_keys_status : "prod.keys: not checked";
}

long Fs_ListDir(const char *path, FsDirFn fn, void *ctx) {
    if (!g_ready) return -1;
    return Fat32_ListDir(path, (Fat32DirFn)fn, ctx);
}

long Fs_ReadFile(const char *path, void *buf, uint32_t max) {
    if (!g_ready) return -1;
    return Fat32_ReadFile(path, buf, max);
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
 * diagnostics. fat32.c keeps its state in globals, so directory listings are
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
    if (Fs_ListDir(dir, NameListCb, &c) < 0) return 0;
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
        long n = Fs_ReadFile(cands[i], g_keybuf, (uint32_t)sizeof(g_keybuf));
        if (n > 0) {
            g_keys = CountKeyEntries(g_keybuf, n);
            char buf[80];
            snprintf(buf, sizeof(buf), "prod.keys: %d keys (%s)", g_keys, cands[i]);
            SetKeysStatus(buf);
            snprintf(diag, sizeof(diag), "found prod.keys at %s (%ld bytes)", cands[i], n);
            SetDiag(diag);
            return;
        }
    }

    /* 2) Enumerate the volume root once. */
    struct NameListCtx root; root.n = 0;
    Fs_ListDir("", NameListCb, &root);

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
            long n = Fs_ReadFile(root.names[i], g_keybuf, (uint32_t)sizeof(g_keybuf));
            if (n > 0) {
                g_keys = CountKeyEntries(g_keybuf, n);
                snprintf(diag, sizeof(diag), "found %s (%ld bytes)", root.names[i], n);
                SetDiag(diag);
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
            long n = Fs_ReadFile(path, g_keybuf, (uint32_t)sizeof(g_keybuf));
            if (n > 0) {
                g_keys = CountKeyEntries(g_keybuf, n);
                snprintf(diag, sizeof(diag), "found %s (%ld bytes)", path, n);
                SetDiag(diag);
                goto done;
            }
        }
    }

    SetKeysStatus("prod.keys: not found");
    snprintf(diag, sizeof(diag), "prod.keys not found; root: %s", g_root_summary);
    SetDiag(diag);
    return;

done:
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "prod.keys: %d keys", g_keys);
        SetKeysStatus(buf);
    }
}

/* Returns 1 if `path` is an existing directory. */
static int DirExists(const char *path) {
    return Fat32_ListDir(path, 0, 0) != -1;
}

int Fs_Mount(int (*tick)(void *ctx, const char *status), void *ctx) {
    SetStatus("initialising USB");
    g_ready = 0;
    g_dev = -1;
    g_dev_count = 0;
    g_vol_size = 0;
    usb_init();

    double deadline = Now() + 6.0;
    int found = 0;
    while (Now() < deadline) {
        if (tick && !tick(ctx, "searching for a USB drive")) return 0;
        /* Use usb_do_poll() (like the working libXenon example) which also runs USBStorage_Init() */
        usb_do_poll();
        /* Count devices and check capacity */
        g_dev_count = 0;
        for (int d = 0; d < 8; ++d) {
            unsigned int ss = 0;
            int cap = USBStorage_Get_Capacity(d, &ss);
            if (cap > 0 && ss == 512u) {
                ++g_dev_count;
                if (!found) {
                    g_dev = d;
                    g_vol_size = (uint64_t)cap * (uint64_t)512u;
                    found = 1;
                }
            }
        }
        if (found) break;
        double t0 = Now();
        while (Now() - t0 < 0.1) usb_do_poll();
    }

    if (!found) {
        if (g_dev_count > 0) {
            SetStatus("USB device found, not 512-byte sectors");
            SetDiag("USB device found but sector size != 512");
        } else {
            SetStatus("no USB mass-storage device");
            SetDiag("no USB mass-storage device detected");
        }
        return 0;
    }

    if (tick) tick(ctx, "mounting FAT32 volume");
    if (!Fat32_Mount(ReadSectors, 0)) {
        SetStatus("not a FAT32 volume");
        SetDiag("FAT32 mount failed");
        return 0;
    }
    g_ready = 1;
    snprintf(g_devtype, sizeof(g_devtype), "USB");
    SetDiag("mounted via USB");

    static const char *cands[] = {
        "roms", "ROMs", "ROMS", "roms/switch", "games", "GAMES", "Games",
        "switch", "SWITCH", "eden/roms", "Eden/roms", "EDEN/ROMS", "roms/switch"
    };
    g_roms[0] = 0;
    g_scan_root_count = 0;
    /* add common roots even if not all exist - but prefer existing ones; also add root */
    for (unsigned i = 0; i < sizeof(cands) / sizeof(cands[0]) && g_scan_root_count < FS_MAX_ROOTS; ++i) {
        int n = 0;
        const char *c = cands[i];
        for (; c[n] && n < FS_PATH_MAX - 1; ++n) g_scan_roots[g_scan_root_count][n] = c[n];
        g_scan_roots[g_scan_root_count][n] = 0;
        ++g_scan_root_count;
        if (DirExists(c) && !g_roms[0]) {
            memcpy(g_roms, g_scan_roots[g_scan_root_count - 1], FS_PATH_MAX);
        }
    }
    /* always include root as a scan root */
    if (g_scan_root_count < FS_MAX_ROOTS) {
        g_scan_roots[g_scan_root_count][0] = 0;
        ++g_scan_root_count;
    }

    char buf[96];
    if (g_roms[0]) {
        snprintf(buf, sizeof(buf), "USB FAT32 ready (%d dev%s)", g_dev_count,
                 g_dev_count == 1 ? "" : "s");
    } else {
        snprintf(buf, sizeof(buf), "USB ok (%d dev%s), no roms/ folder", g_dev_count,
                 g_dev_count == 1 ? "" : "s");
    }
    SetStatus(buf);
    ProbeKeys();
    return 1;
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
