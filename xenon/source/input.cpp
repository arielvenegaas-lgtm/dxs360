/* input.cpp: controller input for libXenon (bare-metal Xbox 360).
 *
 * The D3D9 build polled XInputGetState() (xapilib). libXenon instead runs its
 * own USB stack and exposes the last-known pad report through
 * get_controller_data() (libxenon/drivers/input/input.h). We translate that raw
 * report into the project's PadState, applying the same deadzone/edge rules.
 */
#include <stdint.h>
#include <string.h>

#include <input/input.h>
#include <usb/usbmain.h>

#include "input.h"

#define PAD_COUNT 4

#define STICK_DEADZONE 7849
#define TRIGGER_THRESH 30

static PadState g_cur[PAD_COUNT];
static PadState g_prev[PAD_COUNT];
static int      g_seen[PAD_COUNT]; /* sticky "have we ever heard from port N" */

static float NormalizeStick(int16_t v) {
    int iv = (int)v;
    if (iv > -STICK_DEADZONE && iv < STICK_DEADZONE) return 0.0f;
    if (iv > 0) iv -= STICK_DEADZONE; else iv += STICK_DEADZONE;
    float f = (float)iv / (float)(32767 - STICK_DEADZONE);
    if (f > 1.0f) f = 1.0f;
    if (f < -1.0f) f = -1.0f;
    return f;
}

void Input_Init(void) {
    memset(g_cur, 0, sizeof(g_cur));
    memset(g_prev, 0, sizeof(g_prev));
    memset(g_seen, 0, sizeof(g_seen));

    /* Bring up libXenon's USB stack. Without this the pad driver is never
     * serviced, get_controller_data() never reports a controller, and the
     * game stays in autopilot. */
    usb_init();
    usb_do_poll();
}

void Input_Poll(void) {
    /* Service USB every frame so reports arrive and hotplug is detected. */
    usb_do_poll();

    for (int i = 0; i < PAD_COUNT; ++i) {
        struct controller_data_s d;
        memset(&d, 0, sizeof(d));

        int fresh = get_controller_data(&d, i);
        if (fresh) g_seen[i] = 1;

        PadState p;
        memset(&p, 0, sizeof(p));

        if (g_seen[i]) {
            p.connected = 1;

            if (d.a)     p.buttons |= PAD_A;
            if (d.b)     p.buttons |= PAD_B;
            if (d.x)     p.buttons |= PAD_X;
            if (d.y)     p.buttons |= PAD_Y;
            if (d.start) p.buttons |= PAD_START;
            if (d.back)  p.buttons |= PAD_BACK;
            if (d.lb)    p.buttons |= PAD_LB;
            if (d.rb)    p.buttons |= PAD_RB;
            if (d.up)    p.buttons |= PAD_UP;
            if (d.down)  p.buttons |= PAD_DOWN;
            if (d.left)  p.buttons |= PAD_LEFT;
            if (d.right) p.buttons |= PAD_RIGHT;

            p.lx = NormalizeStick(d.s1_x);
            p.ly = NormalizeStick(d.s1_y);
            p.rx = NormalizeStick(d.s2_x);
            p.ry = NormalizeStick(d.s2_y);
            p.lt = (d.lt > TRIGGER_THRESH) ? d.lt : 0;
            p.rt = (d.rt > TRIGGER_THRESH) ? d.rt : 0;
        }

        p.pressed  = (uint16_t)(p.buttons & ~g_prev[i].buttons);
        p.released = (uint16_t)(~p.buttons &  g_prev[i].buttons);

        g_prev[i] = g_cur[i];
        g_cur[i]  = p;
    }
}

const PadState *Input_GetPad(int index) {
    if (index < 0 || index >= PAD_COUNT) index = 0;
    return &g_cur[index];
}
