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

/* libXenon's usb_do_poll() does two things: it services the USB bus daemons
 * (usb_poll/usb_daemon, which is what actually delivers controller reports) and
 * it calls USBStorage_Init(), which scans every EHCI host port on every call.
 * That scan can hit ehci_msleep(100) whenever a USB device other than mass
 * storage is attached, stalling the game loop by 100+ ms per frame (~8 FPS).
 *
 * We only need controller input, so we drive the bus daemons ourselves and skip
 * the mass-storage probe entirely. usb_poll()/usb_daemon() are declared in
 * <usb/usbd.h>, but that header is not self-contained (it needs CFE's queue_t/
 * physaddr_t), so we forward-declare the opaque bus type and the two functions
 * ourselves. usb_buses[]/usb_buscnt are globals defined in libxenon's usbmain.c
 * and are not declared in any header either. */
extern "C" {
struct usbbus_s;                      /* opaque; real definition in <usb/usbd.h> */
typedef struct usbbus_s usbbus_t;
void usb_poll(usbbus_t *bus);
void usb_daemon(usbbus_t *bus);
extern int       usb_buscnt;
extern usbbus_t *usb_buses[];
}

#include "input.h"

static void UsbPollControllers(void) {
    for (int i = 0; i < usb_buscnt; ++i) {
        usbbus_t *bus = usb_buses[i];
        if (bus) {
            usb_poll(bus);
            usb_daemon(bus);
        }
    }
}

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
    UsbPollControllers();
}

void Input_Poll(void) {
    /* Service USB every frame so reports arrive and hotplug is detected. */
    UsbPollControllers();

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

        /* Edge detection must compare against the PREVIOUS frame's state.
         * g_cur[i] still holds last frame here; g_prev was one frame older,
         * which made every press fire for two frames in a row (so a screen
         * that acts on `accept` "confirmed" itself again and closed). */
        uint16_t prev_buttons = (uint16_t)g_cur[i].buttons;
        p.pressed  = (uint16_t)(p.buttons & ~prev_buttons);
        p.released = (uint16_t)(~p.buttons &  prev_buttons);

        g_prev[i] = g_cur[i];
        g_cur[i]  = p;
    }
}

const PadState *Input_GetPad(int index) {
    if (index < 0 || index >= PAD_COUNT) index = 0;
    return &g_cur[index];
}
