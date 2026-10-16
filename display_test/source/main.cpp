/* main.cpp: minimal standalone Xenos / display bring-up test (libXenon).
 *
 * Purpose: prove, on real hardware, that the reused framebuffer path
 * (src/gfx.cpp -> GPU scan-out) works, with a KNOWN colour pattern and a
 * moving marker so a single photo/screenshot is self-describing:
 *
 *   - eight vertical bars in a fixed, documented order (bar i = i*32 in every
 *     channel, i.e. grey ramp 0x00,0x20,..,0xE0), so a capture can be checked
 *     against an exact expected value;
 *   - a white frame counter and a horizontal sweep bar for a moving element;
 *   - a solid red bar at the very bottom (Y) that must be present but must NOT
 *     cover the bars (guards against a wrong pitch/height).
 *
 * It uses ONLY the repository's Gfx_* API (src/gfx.h); no invented calls.
 * Build: see display_test/Makefile (free60/libxenon toolchain).
 */
#include <stdint.h>
#include <string.h>

#include <ppc/timebase.h>
#include <xenon_soc/xenon_power.h>

#include "gfx.h"

static float NowSeconds(void) {
    return (float)((double)mftb() / (double)PPC_TIMEBASE_FREQ);
}

/* Integer -> decimal, no stdio (the console hook is closed once gfx owns the
 * framebuffer). */
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
    if (!Gfx_Ready(gfx)) {
        /* No display: spin so the failure is obvious (screen stays black). */
        for (;;) { }
    }

    xenon_make_it_faster(XENON_SPEED_FULL);

    float t0 = NowSeconds();
    float prev = t0;

    for (;;) {
        float now = NowSeconds();
        float dt = now - prev;
        prev = now;
        if (dt < 0.0f) dt = 0.0f;

        int W = (int)Gfx_Width(gfx);
        int H = (int)Gfx_Height(gfx);

        /* Background: mid grey. */
        Gfx_Clear(gfx, 0xFF202020u);

        /* Eight vertical bars, known ramp: bar i uses value i*32 per channel.
         * Order (left->right): 0,32,64,96,128,160,192,224. */
        int bw = W / 8;
        for (int i = 0; i < 8; ++i) {
            uint32_t v = (uint32_t)(i * 32);
            uint32_t col = 0xFF000000u | (v << 16) | (v << 8) | v;
            Gfx_FillRect(gfx, i * bw, 0, bw, H - 40, col);
        }

        /* Moving vertical sweep marker (proves the frame loop advances). */
        int sweep = (int)((now - t0) * 120.0f) % (W > 0 ? W : 1);
        Gfx_FillRect(gfx, sweep, 0, 3, H - 40, 0xFFFF00FFu);

        /* Countdown timer in the top-left (seconds since boot). */
        Gfx_FillRect(gfx, 0, 0, 220, 26, 0xC0000000u);
        char b[48]; int n = 0;
        const char *pre = "display_test ";
        while (*pre) b[n++] = *pre++;
        AppendInt(b, &n, (int)(now - t0));
        b[n++] = 's'; b[n] = 0;
        Gfx_DrawText(gfx, 8, 6, 2, 0xFFFFFFFFu, b);

        /* Solid red strip along the bottom that must NOT cover the bars. */
        Gfx_FillRect(gfx, 0, H - 40, W, 40, 0xFFE00000u);
        Gfx_DrawText(gfx, 8, H - 30, 2, 0xFFFFFFFFu,
                     "KNOWN PATTERN: bars 0,32,64,96,128,160,192,224");

        Gfx_Present(gfx);
    }

    return 0; /* unreachable */
}
