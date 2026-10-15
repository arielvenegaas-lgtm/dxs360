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
}

#include "fs.h"
#include "fat32.h"

static int  g_dev  = -1;
static int  g_dev_count = 0;
static uint64_t g_vol_size = 0;
static int  g_ready = 0;
static char g_roms[FS_PATH_MAX];
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

/* Looks for a prod.keys file under keys/ and reports how many entries it has.
 * Informational only: this build does not use keys. */
static void ProbeKeys(void) {
    static const char *cands[] = {
        "keys/prod.keys", "eden/keys/prod.keys", "Eden/keys/prod.keys", "prod.keys"
    };
    g_keys = -1;
    for (unsigned i = 0; i < sizeof(cands) / sizeof(cands[0]); ++i) {
        long n = Fs_ReadFile(cands[i], g_keybuf, (uint32_t)sizeof(g_keybuf));
        if (n > 0) {
            g_keys = CountKeyEntries(g_keybuf, n);
            char buf[64];
            snprintf(buf, sizeof(buf), "prod.keys: %d keys", g_keys);
            SetKeysStatus(buf);
            return;
        }
    }
    SetKeysStatus("prod.keys: not found");
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
        } else {
            SetStatus("no USB mass-storage device");
        }
        return 0;
    }

    if (tick) tick(ctx, "mounting FAT32 volume");
    if (!Fat32_Mount(ReadSectors, 0)) {
        SetStatus("not a FAT32 volume");
        return 0;
    }
    g_ready = 1;

    static const char *cands[] = {
        "roms", "eden/roms", "Eden/roms", "EDEN/ROMS", "ROMs", "ROMS", "roms/switch"
    };
    g_roms[0] = 0;
    for (unsigned i = 0; i < sizeof(cands) / sizeof(cands[0]); ++i) {
        if (DirExists(cands[i])) {
            int n = 0;
            for (; cands[i][n] && n < FS_PATH_MAX - 1; ++n) g_roms[n] = cands[i][n];
            g_roms[n] = 0;
            break;
        }
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
