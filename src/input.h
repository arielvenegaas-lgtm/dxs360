/* input.h: Xbox 360 controller input (XInput).
 *
 * Kept in its own translation unit that does NOT include <xtl.h>; the XInput
 * entry point is declared with a local, hardware-verified ABI (see input.cpp).
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PAD_A      = 0x0001,
    PAD_B      = 0x0002,
    PAD_X      = 0x0004,
    PAD_Y      = 0x0008,
    PAD_START  = 0x0010,
    PAD_BACK   = 0x0020,
    PAD_LB     = 0x0040,
    PAD_RB     = 0x0080,
    PAD_UP     = 0x0100,
    PAD_DOWN   = 0x0200,
    PAD_LEFT   = 0x0400,
    PAD_RIGHT  = 0x0800
};

typedef struct {
    int      connected;
    float    lx, ly, rx, ry; /* left/right sticks, -1..1 (deadzone applied) */
    uint8_t  lt, rt;         /* triggers, 0..255 */
    uint16_t buttons;        /* held this frame */
    uint16_t pressed;        /* went down this frame */
    uint16_t released;       /* went up this frame */
} PadState;

void Input_Init(void);
void Input_Poll(void);                 /* call once per frame */
const PadState *Input_GetPad(int index); /* index 0..3; never NULL */

#ifdef __cplusplus
}
#endif
