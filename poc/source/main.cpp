/* main.cpp: Hollow-Knight sprite PoC for libXenon (bare-metal Xbox 360).
 *
 * Loads ONE genuine sprite that the user extracted from their own local game
 * install (tools/hk_extract.py -> .spr) off a FAT32 USB stick and displays it
 * through the scan-out framebuffer. NO game asset is compiled into this binary.
 *
 * USB note: USBStorage_Init() walks every EHCI host port and may reset a port
 * that holds a HID pad, so a controller can drop offline while it runs. We do
 * mass-storage discovery in a bounded phase BEFORE the pad driver is started,
 * and only re-probe slowly if no drive turned up. That is also why the game
 * build (xenon/source/input.cpp) never calls USBStorage_Init at all.
 */
#include <stdint.h>
#include <string.h>

#include <ppc/timebase.h>
#include <xenon_soc/xenon_power.h>
#include <usb/usbmain.h> /* usb_init */

/* libXenon's usb_poll/usb_daemon live in <usb/usbd.h>, which is not
 * self-contained, and the mass-storage entry points are not in usbmain.h.
 * Forward-declare exactly what we use. */
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

#include "gfx.h"
#include "input.h"
#include "img.h"
#include "fat32.h"

#define FILE_NAME "knight_icon.spr"

static uint8_t  g_filebuf[4u << 20]; /* 4 MiB is plenty for a .spr */

/* Discovery state. */
static enum {
    ST_SEARCHING,  /* probing for a mass-storage device */
    ST_BADSECT,    /* drive found but not 512-byte sectors */
    ST_MOUNTFAIL,  /* drive found, FAT32 mount failed     */
    ST_NOFILE,     /* FAT32 mounted, FILE_NAME missing     */
    ST_BADSPR,     /* file read, decode failed             */
    ST_OK          /* sprite decoded and ready             */
} g_state = ST_SEARCHING;

static int      g_dev = -1;
static uint32_t g_secsz = 0;
static long     g_filelen = 0;

static int ReadSectors(void *ctx, uint32_t lba, uint32_t count, void *buf) {
    (void)ctx;
    return USBStorage_Read_Sectors(g_dev, (unsigned)lba, (unsigned)count, buf) ? 1 : 0;
}

static void PollBuses(void) {
    for (int i = 0; i < usb_buscnt; ++i) {
        usbbus_t *b = usb_buses[i];
        if (b) { usb_poll(b); usb_daemon(b); }
    }
}

static float NowSeconds(void) {
    return (float)((double)mftb() / (double)PPC_TIMEBASE_FREQ);
}

/* Busy-wait (no sleeping primitive in the PoC; mftb is monotonic). */
static void WaitMs(int ms) {
    double t0 = (double)mftb() / (double)PPC_TIMEBASE_FREQ;
    for (;;) {
        double t = (double)mftb() / (double)PPC_TIMEBASE_FREQ;
        if (t - t0 >= (double)ms / 1000.0) break;
        PollBuses(); /* keep the bus serviced while we wait */
    }
}

/* One probe pass: poll the buses, scan for a mounted mass-storage device,
 * then try to read and decode FILE_NAME from its root. */
static void ProbeOnce(void) {
    PollBuses();
    USBStorage_Init();

    for (int d = 0; d < 4; ++d) {
        unsigned int ss = 0;
        if (USBStorage_Get_Capacity(d, &ss) <= 0) continue;

        g_dev  = d;
        g_secsz = ss;
        if (ss != 512u) { g_state = ST_BADSECT; return; }

        if (!Fat32_Mount(ReadSectors, 0)) { g_state = ST_MOUNTFAIL; return; }

        g_filelen = Fat32_ReadFile(FILE_NAME, g_filebuf, (uint32_t)sizeof(g_filebuf));
        if (g_filelen <= 0) { g_state = ST_NOFILE; return; }
        return; /* decoded in the loop once it sees bytes */
    }
    g_state = ST_SEARCHING;
}

/* Minimal integer -> string (no printf: the console hook is closed by gfx). */
static void Append(char *b, int *n, const char *s) {
    while (*s) b[(*n)++] = *s++;
}
static void AppendInt(char *b, int *n, int v) {
    char t[12]; int m = 0;
    if (v == 0) { b[(*n)++] = '0'; return; }
    if (v < 0) { b[(*n)++] = '-'; v = -v; }
    while (v > 0) { t[m++] = (char)('0' + v % 10); v /= 10; }
    while (m > 0) b[(*n)++] = t[--m];
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    GfxContext *gfx = Gfx_Create();
    if (!Gfx_Ready(gfx)) { for (;;) { } }

    xenon_make_it_faster(XENON_SPEED_FULL);

    /* Bring up EHCI + OHCI ourselves, then hunt for the stick for a few
     * seconds BEFORE the pad driver starts, so USBStorage_Init's port resets
     * do not leave a controller half-enumerated. */
    usb_init();

    float deadline = NowSeconds() + 6.0f;
    while (NowSeconds() < deadline) {
        ProbeOnce();
        if (g_state == ST_BADSECT || g_state == ST_MOUNTFAIL || g_state == ST_NOFILE)
            break;
        WaitMs(100);
    }

    /* Pads now. usb_init() is a no-op on the second call. */
    Input_Init();
    for (int i = 0; i < 30; ++i) WaitMs(16); /* let HID re-enumerate */

    Image img;
    memset(&img, 0, sizeof(img));
    int scale = 6;
    float prev = NowSeconds();
    float fps = 0.0f; int frames = 0; float win = prev;
    float last_probe = prev;

    for (;;) {
        /* If we never found a drive, keep re-probing slowly so a stick
         * plugged in after boot (or a slow enumeration) still shows up.
         * The long gaps let the pad settle between port resets. */
        if (g_state == ST_SEARCHING && (prev - last_probe) >= 2.0f) {
            last_probe = prev;
            ProbeOnce();
        }

        /* Decode as soon as a file lands (ProbeOnce stops short of that). */
        if (g_state == ST_SEARCHING && g_filelen > 0) {
            if (Img_LoadSpr(g_filebuf, (size_t)g_filelen, &img))
                g_state = ST_OK;
            else
                g_state = ST_BADSPR;
        }

        Input_Poll();
        const PadState *pad = Input_GetPad(0);
        if (pad) {
            if (pad->pressed & PAD_A) scale += 1;
            if ((pad->pressed & PAD_B) && scale > 1) scale -= 1;
        }

        float now = NowSeconds();
        prev = now;
        ++frames;
        if (now - win >= 0.5f) { fps = (float)frames / (now - win); frames = 0; win = now; }

        Gfx_Clear(gfx, 0xFF0E0E16u);
        int W = (int)Gfx_Width(gfx), H = (int)Gfx_Height(gfx);

        if (g_state == ST_OK && img.pixels) {
            int w = (int)img.w * scale;
            int h = (int)img.h * scale;
            int x = (W - w) / 2, y = (H - h) / 2;
            Gfx_FillRect(gfx, x - 4, y - 4, w + 8, 4, 0xFF3A3A50u);
            Gfx_FillRect(gfx, x - 4, y + h, w + 8, 4, 0xFF3A3A50u);
            Gfx_FillRect(gfx, x - 4, y, 4, h, 0xFF3A3A50u);
            Gfx_FillRect(gfx, x + w, y, 4, h, 0xFF3A3A50u);
            Gfx_DrawImage(gfx, x, y, w, h, img.pixels, (int)img.w, (int)img.h);

            Gfx_DrawText(gfx, 16, 12, 3, 0xFFEFEFFF, "HOLLOW KNIGHT SPRITE POC");
            char b[96]; int n = 0;
            Append(b, &n, FILE_NAME "  ");
            AppendInt(b, &n, (int)img.w); Append(b, &n, "x"); AppendInt(b, &n, (int)img.h);
            Append(b, &n, "  x"); AppendInt(b, &n, scale);
            Append(b, &n, "  "); AppendInt(b, &n, (int)g_filelen); Append(b, &n, " bytes");
            b[n] = 0;
            Gfx_DrawText(gfx, 16, 48, 2, 0xFFB8B8CCu, b);
            Gfx_DrawText(gfx, 16, 72, 2, 0xFF8080A0u, "A = zoom in   B = zoom out");
            char f[48]; n = 0;
            Append(f, &n, "FPS "); AppendInt(f, &n, (int)(fps + 0.5f));
            f[n] = 0;
            Gfx_DrawText(gfx, 16, 96, 2, 0xFF6080A0u, f);
        } else {
            Gfx_DrawText(gfx, 40, 32, 3, 0xFFFF7A7Au, "SPRITE POC: no asset loaded");
            Gfx_DrawText(gfx, 16, 80, 2, 0xFFC0C0C0u, "status:");

            char m[96]; int n = 0;
            switch (g_state) {
            case ST_SEARCHING:
                Append(m, &n, "searching USB (try a FAT32 stick, MBR)");
                break;
            case ST_BADSECT:
                Append(m, &n, "drive seen, unsupported sector size ");
                AppendInt(m, &n, (int)g_secsz);
                break;
            case ST_MOUNTFAIL:
                Append(m, &n, "FAT32 mount failed (drive is not FAT32/MBR)");
                break;
            case ST_NOFILE:
                Append(m, &n, FILE_NAME " not in the USB root");
                break;
            case ST_BADSPR:
                Append(m, &n, "could not decode " FILE_NAME);
                break;
            default:
                break;
            }
            m[n] = 0;
            Gfx_DrawText(gfx, 16, 112, 2, 0xFFB8B8CCu, m);

            char d[64]; n = 0;
            Append(d, &n, "mass-storage device: ");
            Append(d, &n, (g_dev >= 0) ? "YES" : "no");
            if (g_dev >= 0) {
                Append(d, &n, "  sectorsize ");
                AppendInt(d, &n, (int)g_secsz);
            }
            d[n] = 0;
            Gfx_DrawText(gfx, 16, 140, 2, 0xFF8080A0u, d);

            char f[48]; n = 0;
            Append(f, &n, "FPS "); AppendInt(f, &n, (int)(fps + 0.5f));
            f[n] = 0;
            Gfx_DrawText(gfx, 16, H - 32, 2, 0xFF6080A0u, f);
        }

        Gfx_Present(gfx);
    }

    return 0;
}
