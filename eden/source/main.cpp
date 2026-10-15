/* main.cpp: Eden360 frontend entry point (libXenon, bare-metal Xbox 360).
 *
 * Boot is deliberately identical to the proven game build (xenon/source):
 *   Gfx_Create -> xenon_make_it_faster -> Input_Init -> UI loop.
 *
 * We do NOT touch USB mass storage at boot. libXenon's USB mass-storage probe
 * (usb_init + USBStorage_Init) hangs the console on some units when the ELF is
 * launched from Aurora (the game only works because it never calls it). Storage
 * is therefore mounted lazily, on demand, from the UI (see app.cpp).
 */
#include <ppc/timebase.h>
#include <xenon_soc/xenon_power.h>
#include <xenon_smc/xenon_smc.h>

#include <string.h>

#include "gfx.h"
#include "app.h"
#include "ui.h"

static float NowSeconds(void) {
    return (float)((double)mftb() / (double)PPC_TIMEBASE_FREQ);
}

/* The library table is large; keep the app state in BSS, not on the stack. */
static EdenApp g_app;

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    GfxContext *gfx = Gfx_Create();
    if (!Gfx_Ready(gfx)) { for (;;) { } }

    /* Run the CPU at full clock (the bootloader may leave it throttled). */
    xenon_make_it_faster(XENON_SPEED_FULL);

    /* Bring up controller input the same way the working game does. This also
     * initialises libXenon's USB stack. Nothing else USB-related is done here. */
    Input_Init();

    EdenApp *app = &g_app;
    App_Init(app, gfx);

    UI_DrawLoading(app, "Starting up...", 0.0f);
    Gfx_Present(gfx);

    float prev = NowSeconds();
    float reboot_hold = 0.0f; /* seconds Back+Start held */
    for (;;) {
        Input_Poll();
        const PadState *pad = Input_GetPad(0);

        float now = NowSeconds();
        float dt = now - prev;
        prev = now;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.1f) dt = 0.1f;

        /* Hold Back+Start for ~1 second to soft-reboot. This is the only way
         * back out of a bare-metal ELF launched from Aurora's file manager
         * (same behaviour as the Aurora Bricks build). */
        PadState view;
        if (pad) view = *pad; else memset(&view, 0, sizeof(view));

        int exit_combo = pad && pad->connected &&
            (pad->buttons & (PAD_BACK | PAD_START)) == (PAD_BACK | PAD_START);
        if (exit_combo) {
            if (reboot_hold == 0.0f) App_ShowMessage(app, "Holding to return to Aurora...");
            reboot_hold += dt;
            if (reboot_hold >= 1.0f) xenon_smc_power_reboot();
            /* Don't let the Start press open the overlay while exiting. */
            view.buttons = (uint16_t)(view.buttons & ~(PAD_BACK | PAD_START));
            view.pressed = (uint16_t)(view.pressed & ~(PAD_BACK | PAD_START));
        } else {
            reboot_hold = 0.0f;
        }

        App_Update(app, &view, dt);
        UI_Draw(app);
        Gfx_Present(gfx);
    }

    return 0; /* unreachable */
}
