/* input.cpp: XInput polling with edge detection.
 *
 * The Xbox 360 XInput ABI is declared locally (struct layout verified on retail
 * hardware in Stu1dd3m0n9/xbox-360-port-enablers, MIT). XInputGetState is
 * provided by xapilib, which the project links.
 */
#include <stdint.h>
#include <string.h>

#include "input.h"

#define PAD_COUNT 4

typedef struct {
    uint16_t b;
    uint8_t  lt, rt;
    int16_t  lx, ly, rx, ry;
} XInputGamepad;

typedef struct {
    uint32_t      packet;
    XInputGamepad gamepad;
} XInputState;

/* xapilib export. Returns 0 (ERROR_SUCCESS) on success. */
extern "C" unsigned int XInputGetState(unsigned int index, XInputState *state);

/* Hardware button bits (XInput gamepad). */
#define XI_DPAD_UP    0x0001u
#define XI_DPAD_DOWN  0x0002u
#define XI_DPAD_LEFT  0x0004u
#define XI_DPAD_RIGHT 0x0008u
#define XI_START      0x0010u
#define XI_BACK       0x0020u
#define XI_LB         0x0100u
#define XI_RB         0x0200u
#define XI_A          0x1000u
#define XI_B          0x2000u
#define XI_X          0x4000u
#define XI_Y          0x8000u

#define STICK_DEADZONE 7849
#define TRIGGER_THRESH 30

static PadState g_cur[PAD_COUNT];
static PadState g_prev[PAD_COUNT];

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
}

void Input_Poll(void) {
    for (int i = 0; i < PAD_COUNT; ++i) {
        XInputState st;
        memset(&st, 0, sizeof(st));

        PadState p;
        memset(&p, 0, sizeof(p));

        if (XInputGetState((unsigned int)i, &st) == 0) {
            uint16_t b = st.gamepad.b;
            p.connected = 1;
            if (b & XI_A)      p.buttons |= PAD_A;
            if (b & XI_B)      p.buttons |= PAD_B;
            if (b & XI_X)      p.buttons |= PAD_X;
            if (b & XI_Y)      p.buttons |= PAD_Y;
            if (b & XI_START)  p.buttons |= PAD_START;
            if (b & XI_BACK)   p.buttons |= PAD_BACK;
            if (b & XI_LB)     p.buttons |= PAD_LB;
            if (b & XI_RB)     p.buttons |= PAD_RB;
            if (b & XI_DPAD_UP)    p.buttons |= PAD_UP;
            if (b & XI_DPAD_DOWN)  p.buttons |= PAD_DOWN;
            if (b & XI_DPAD_LEFT)  p.buttons |= PAD_LEFT;
            if (b & XI_DPAD_RIGHT) p.buttons |= PAD_RIGHT;

            p.lx = NormalizeStick(st.gamepad.lx);
            p.ly = NormalizeStick(st.gamepad.ly);
            p.rx = NormalizeStick(st.gamepad.rx);
            p.ry = NormalizeStick(st.gamepad.ry);
            p.lt = (st.gamepad.lt > TRIGGER_THRESH) ? st.gamepad.lt : 0;
            p.rt = (st.gamepad.rt > TRIGGER_THRESH) ? st.gamepad.rt : 0;
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
