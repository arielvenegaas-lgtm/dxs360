/* main.cpp: Eden360 frontend entry point (libXenon, bare-metal Xbox 360).
 *
 * Boot order matters: the USB mass-storage probe resets EHCI host ports, so it
 * runs BEFORE the pad driver is started (otherwise a controller can be left
 * half-enumerated). The loading screen animates while that probe runs.
 */
#include <ppc/timebase.h>
#include <xenon_soc/xenon_power.h>

#include "gfx.h"
#include "app.h"
#include "ui.h"
#include "fs.h"

static float NowSeconds(void) {
    return (float)((double)mftb() / (double)PPC_TIMEBASE_FREQ);
}

typedef struct { EdenApp *a; float t; } LoadCtx;

static int LoadTick(void *ctx, const char *status) {
    LoadCtx *lc = (LoadCtx *)ctx;
    lc->t += 0.02f;
    UI_DrawLoading(lc->a, status, lc->t);
    Gfx_Present(lc->a->gfx);
    return 1;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    GfxContext *gfx = Gfx_Create();
    if (!Gfx_Ready(gfx)) { for (;;) { } }

    xenon_make_it_faster(XENON_SPEED_FULL);

    EdenApp app;
    App_Init(&app, gfx);

    LoadCtx lc;
    lc.a = &app;
    lc.t = 0.0f;
    UI_DrawLoading(&app, "Starting up...", 0.0f);
    Gfx_Present(gfx);

    int ok = Fs_Mount(LoadTick, &lc);
    app.fs_ready = ok;

    UI_DrawLoading(&app, ok ? "Storage ready" : "No USB storage found", lc.t + 0.4f);
    Gfx_Present(gfx);

    /* Pads only now, after the mass-storage port scan has finished. */
    Input_Init();

    if (app.set.scan_on_start) App_Rescan(&app);

    float prev = NowSeconds();
    for (;;) {
        Input_Poll();
        const PadState *pad = Input_GetPad(0);

        float now = NowSeconds();
        float dt = now - prev;
        prev = now;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.1f) dt = 0.1f;

        App_Update(&app, pad, dt);
        UI_Draw(&app);
        Gfx_Present(gfx);
    }

    return 0; /* unreachable */
}
