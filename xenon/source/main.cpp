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

    for (;;) {
        Input_Poll();
        const PadState *pad = Input_GetPad(0);

        float now = NowSeconds();
        float dt = now - prev;
        prev = now;

        Game_Update(&game, pad, dt);

        /* Recompute the displayed FPS about twice a second. */
        ++fps_frames;
        float elapsed = now - fps_window;
        if (elapsed >= 0.5f) {
            game.fps = (float)fps_frames / elapsed;
            fps_frames = 0;
            fps_window = now;
        }

        int pitch = 0;
        uint32_t *fb = Gfx_Lock(gfx, &pitch);
        if (fb) {
            Game_Draw(&game, gfx);
        }
        Gfx_Present(gfx);
    }

    return 0; /* unreachable */
}
