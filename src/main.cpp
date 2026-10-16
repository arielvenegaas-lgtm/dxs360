/* main.cpp: Aurora Bricks - entry point and main loop.
 *
 * Xbox 360 title. Compiled with the RXDK-360 clang toolset and linked against
 * your licensed Xbox 360 XDK. The result is a real, unsigned XEX (dev-signed
 * with XexTool) that Aurora can launch as default.xex on RGH/JTAG consoles.
 *
 * NOTE: returning from main() calls HalReturnToFirmware(0), which powers the
 * console off, so the loop intentionally never exits.
 */
#include <xtl.h>

#include "gfx.h"
#include "input.h"
#include "game.h"

extern "C" int DbgPrint(const char *fmt, ...);

static float NowSeconds(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    if (!f.QuadPart) return 0.0f;
    return (float)t.QuadPart / (float)f.QuadPart;
}

int main() {
    GfxContext *gfx = Gfx_Create();
    if (!Gfx_Ready(gfx)) {
        DbgPrint("[aurora-bricks] no D3D device; halting\n");
        for (;;) Sleep(1000);
    }

    Input_Init();

    Game game;
    Game_Init(&game, Gfx_Width(gfx), Gfx_Height(gfx));

    DbgPrint("[aurora-bricks] up %ux%u\n",
             (unsigned)Gfx_Width(gfx), (unsigned)Gfx_Height(gfx));

    float prev = NowSeconds();

    for (;;) {
        Input_Poll();
        const PadState *pad = Input_GetPad(0);

        float now = NowSeconds();
        float dt = now - prev;
        prev = now;

        Game_Update(&game, pad, dt);

        int pitch = 0;
        uint32_t *fb = Gfx_Lock(gfx, &pitch);
        if (fb) {
            Game_Draw(&game, gfx);
        }
        Gfx_Present(gfx);
    }

    return 0; /* unreachable */
}
