/* main.cpp: Hollow-Knight sprite PoC for libXenon (bare-metal Xbox 360).
 *
 * Loads ONE genuine sprite that the user extracted from their own local game
 * install (tools/hk_extract.py -> .spr) off a FAT32 USB stick and displays it
 * through the scan-out framebuffer. NO game asset is compiled into this binary.
 *
 * Pipeline:
 *   usb_init() -> find an EHCI mass-storage device -> Fat32_Mount on top of
 *   USBStorage_Read_Sectors() -> Fat32_ReadFile("knight_icon.spr") ->
 *   Img_LoadSpr() -> Gfx_DrawImage(). */
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

static int g_dev = -1;

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

static int FindUsbStorage(uint32_t *sector_size) {
    for (int tries = 0; tries < 240; ++tries) {
        PollBuses();
        USBStorage_Init();
        for (int d = 0; d < 4; ++d) {
            unsigned int ss = 0;
            if (USBStorage_Get_Capacity(d, &ss) > 0) {
                g_dev = d;
                if (sector_size) *sector_size = ss;
                return d;
            }
        }
    }
    return -1;
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

    /* Brings up EHCI + OHCI (usb_init -> EHCI_Init) so mass storage can mount. */
    Input_Init();

    uint32_t sector_size = 0;
    int dev = FindUsbStorage(&sector_size);

    Image img;
    memset(&img, 0, sizeof(img));
    int mounted = 0;
    long file_len = 0;
    int  mounted_fs = 0;

    if (dev >= 0 && sector_size == 512) {
        if (Fat32_Mount(ReadSectors, 0)) {
            mounted_fs = 1;
            file_len = Fat32_ReadFile(FILE_NAME, g_filebuf, (uint32_t)sizeof(g_filebuf));
            if (file_len > 0 && Img_LoadSpr(g_filebuf, (size_t)file_len, &img))
                mounted = 1;
        }
    }

    int scale = 6;
    float prev = NowSeconds();
    float fps = 0.0f; int frames = 0; float win = prev;

    for (;;) {
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

        if (mounted) {
            int w = (int)img.w * scale;
            int h = (int)img.h * scale;
            int x = (W - w) / 2, y = (H - h) / 2;
            /* frame */
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
            Append(b, &n, "  "); AppendInt(b, &n, (int)file_len); Append(b, &n, " bytes");
            b[n] = 0;
            Gfx_DrawText(gfx, 16, 48, 2, 0xFFB8B8CCu, b);
            Gfx_DrawText(gfx, 16, 72, 2, 0xFF8080A0u, "A = zoom in   B = zoom out");
            char f[48]; n = 0;
            Append(f, &n, "FPS "); AppendInt(f, &n, (int)(fps + 0.5f));
            f[n] = 0;
            Gfx_DrawText(gfx, 16, 96, 2, 0xFF6080A0u, f);
        } else {
            Gfx_DrawText(gfx, 40, 40, 3, 0xFFFF7A7Au, "SPRITE POC: no asset loaded");
            if (dev < 0)
                Gfx_DrawText(gfx, 40, 90, 2, 0xFFC0C0C0u, "no USB mass-storage device found");
            else if (sector_size != 512) {
                char m[64]; int n = 0;
                Append(m, &n, "unsupported sector size ");
                AppendInt(m, &n, (int)sector_size);
                m[n] = 0;
                Gfx_DrawText(gfx, 40, 90, 2, 0xFFC0C0C0u, m);
            } else if (!mounted_fs)
                Gfx_DrawText(gfx, 40, 90, 2, 0xFFC0C0C0u, "FAT32 mount failed (need FAT32, MBR)");
            else if (file_len <= 0)
                Gfx_DrawText(gfx, 40, 90, 2, 0xFFC0C0C0u, "put " FILE_NAME " in the USB root");
            else
                Gfx_DrawText(gfx, 40, 90, 2, 0xFFC0C0C0u, "failed to decode " FILE_NAME);
        }

        Gfx_Present(gfx);
    }

    return 0;
}
