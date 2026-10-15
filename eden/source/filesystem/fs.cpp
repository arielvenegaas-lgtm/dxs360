/* fs.cpp: see fs.h. */
#include <stdint.h>
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
static int  g_ready = 0;
static char g_roms[FS_PATH_MAX];
static char g_status[64];

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

int Fs_Ready(void) { return g_ready; }
const char *Fs_RomsDir(void) { return g_roms; }
const char *Fs_Status(void) { return g_status[0] ? g_status : "not mounted"; }

long Fs_ListDir(const char *path, FsDirFn fn, void *ctx) {
    if (!g_ready) return -1;
    return Fat32_ListDir(path, (Fat32DirFn)fn, ctx);
}

long Fs_ReadFile(const char *path, void *buf, uint32_t max) {
    if (!g_ready) return -1;
    return Fat32_ReadFile(path, buf, max);
}

/* Returns 1 if `path` is an existing directory. */
static int DirExists(const char *path) {
    return Fat32_ListDir(path, 0, 0) != -1;
}

int Fs_Mount(int (*tick)(void *ctx, const char *status), void *ctx) {
    SetStatus("initialising USB");
    usb_init();

    /* Probe for a mass-storage device for a few seconds. USBStorage_Init()
     * resets EHCI ports, which is why the caller runs this before starting the
     * pad driver. */
    double deadline = Now() + 6.0;
    int found = 0;
    while (Now() < deadline) {
        if (tick && !tick(ctx, "searching for a USB drive")) return 0;
        PollBuses();
        USBStorage_Init();
        for (int d = 0; d < 4; ++d) {
            unsigned int ss = 0;
            if (USBStorage_Get_Capacity(d, &ss) > 0) {
                g_dev = d;
                if (ss != 512u) {
                    SetStatus("drive is not 512-byte sectors");
                    g_dev = -1;
                    return 0;
                }
                found = 1;
                break;
            }
        }
        if (found) break;
        /* small pause without a sleep primitive */
        double t0 = Now();
        while (Now() - t0 < 0.1) PollBuses();
    }

    if (!found) { SetStatus("no USB mass-storage device"); return 0; }

    if (tick) tick(ctx, "mounting FAT32 volume");
    if (!Fat32_Mount(ReadSectors, 0)) { SetStatus("not a FAT32 volume"); return 0; }
    g_ready = 1;

    /* Resolve the roms/ directory: the package may be extracted at the volume
     * root or under an "eden/" folder. */
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

    SetStatus(g_roms[0] ? "USB FAT32 ready" : "USB ok, no roms/ folder");
    return 1;
}
