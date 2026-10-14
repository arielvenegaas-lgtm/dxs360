/* main.cpp: Aurora Bricks - libXenon (bare-metal) entry point and main loop.
 *
 * Built with the xenon- toolchain against libXenon, this produces
 * aurora_bricks.elf32, a bare-metal PPC executable you launch from Aurora's
 * File Manager or from XeLL. It renders through the GPU scan-out framebuffer
 * (see gfx.cpp) and reads the pad through libXenon's USB input stack
 * (see input.cpp).
 */
#include <ppc/timebase.h>
#include <xenon_soc/xenon_power.h>

#include "gfx.h"
#include "input.h"
#include "game.h"

static float NowSeconds(void) {
    return (float)((double)mftb() / (double)PPC_TIMEBASE_FREQ);
}

/* --- per-stage timing (temporary perf diagnostics) -------------------------
 * We time each phase of the frame and show the per-frame average, in ms, in the
 * bottom-left corner. This tells us where the frame budget actually goes. */
#define TB_PER_MS ((double)PPC_TIMEBASE_FREQ / 1000.0)

static int TbToMs(unsigned long long ticks, int frames) {
    if (frames < 1) return 0;
    double ms = ((double)ticks / (double)frames) / TB_PER_MS;
    if (ms < 0.0) ms = 0.0;
    if (ms > 999.0) ms = 999.0;
    return (int)(ms + 0.5);
}

static void AppendStr(char **p, const char *s) {
    while (*s) *(*p)++ = *s++;
}

static void AppendUInt(char **p, unsigned v) {
    char tmp[12];
    int n = 0;
    if (v == 0) { *(*p)++ = '0'; return; }
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) *(*p)++ = tmp[--n];
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    GfxContext *gfx = Gfx_Create();
    if (!Gfx_Ready(gfx)) {
        for (;;) { }
    }

    /* Run the CPU at full clock (the bootloader may leave it throttled). */
    xenon_make_it_faster(XENON_SPEED_FULL);

    Input_Init();

    Game game;
    Game_Init(&game, Gfx_Width(gfx), Gfx_Height(gfx));

    float prev = NowSeconds();
    float fps_window = prev;
    int   fps_frames = 0;

    unsigned long long acc_usb = 0, acc_upd = 0, acc_drw = 0, acc_blt = 0;
    int ms_usb = 0, ms_upd = 0, ms_drw = 0, ms_blt = 0;

    for (;;) {
        unsigned long long t0 = mftb();
        Input_Poll();
        unsigned long long t1 = mftb();
        const PadState *pad = Input_GetPad(0);

        float now = NowSeconds();
        float dt = now - prev;
        prev = now;

        Game_Update(&game, pad, dt);
        unsigned long long t2 = mftb();

        /* Recompute the displayed FPS about twice a second. */
        ++fps_frames;
        float elapsed = now - fps_window;
        if (elapsed >= 0.5f) {
            game.fps = (float)fps_frames / elapsed;
            ms_usb = TbToMs(acc_usb, fps_frames);
            ms_upd = TbToMs(acc_upd, fps_frames);
            ms_drw = TbToMs(acc_drw, fps_frames);
            ms_blt = TbToMs(acc_blt, fps_frames);
            fps_frames = 0;
            fps_window = now;
            acc_usb = acc_upd = acc_drw = acc_blt = 0;
        }

        int pitch = 0;
        uint32_t *fb = Gfx_Lock(gfx, &pitch);
        if (fb) {
            Game_Draw(&game, gfx);
        }
        unsigned long long t3 = mftb();

        Gfx_Present(gfx);
        unsigned long long t4 = mftb();

        acc_usb += t1 - t0;
        acc_upd += t2 - t1;
        acc_drw += t3 - t2;
        acc_blt += t4 - t3;

        /* Timing readout: usb / update / draw / blit, milliseconds per frame. */
        {
            char line[48];
            char *p = line;
            AppendStr(&p, "US"); AppendUInt(&p, (unsigned)ms_usb);
            AppendStr(&p, " UP"); AppendUInt(&p, (unsigned)ms_upd);
            AppendStr(&p, " DR"); AppendUInt(&p, (unsigned)ms_drw);
            AppendStr(&p, " BL"); AppendUInt(&p, (unsigned)ms_blt);
            *p = 0;
            Gfx_DrawText(gfx, 4, (int)Gfx_Height(gfx) - 12, 1, 0xFFFFFFFFu, line);
        }
    }

    return 0; /* unreachable */
}
