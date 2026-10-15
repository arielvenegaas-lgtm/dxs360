/* test_input.cpp: host-side unit tests for the controller edge detection in
 * xenon/source/input.cpp.
 *
 * Regression guard: `pressed` must be true for exactly ONE frame per press.
 * An earlier bug compared against a state two frames old, so every press fired
 * for two frames -- opening the About/System screens "confirmed" itself again
 * on the next frame and closed, and B/Back misbehaved.
 *
 * Build (from the repo root):
 *   g++ -std=c++11 -Wall -I eden/tests/stub -I src \
 *       eden/tests/test_input.cpp xenon/source/input.cpp -o /tmp/test_input
 *   /tmp/test_input
 */
#include <cstdio>
#include <cstring>

#include <input/input.h>

#include "input.h"

/* ---- libXenon stubs so the real input.cpp links on the host ---- */
extern "C" {
struct usbbus_s;
typedef struct usbbus_s usbbus_t;

int       usb_buscnt = 0;
usbbus_t *usb_buses[1] = { 0 };
void usb_init(void) {}
void usb_poll(usbbus_t *bus)   { (void)bus; }
void usb_daemon(usbbus_t *bus) { (void)bus; }

/* Scripted controller: one report per Input_Poll() call, indexed by frame. */
static struct controller_data_s g_script[16];
static int                      g_frame = 0;

int get_controller_data(struct controller_data_s *data, int index) {
    if (index != 0) { std::memset(data, 0, sizeof(*data)); return 0; }
    int f = (g_frame < 16) ? g_frame : 15;
    *data = g_script[f];
    ++g_frame;
    return 1;
}
}

static int g_fail = 0;

#define CHECK(cond) \
    do { if (!(cond)) { \
        std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++g_fail; } } while (0)

/* Sets the buttons reported on frame `f`. */
static void Frame(int f, int a, int b, int start, int back) {
    std::memset(&g_script[f], 0, sizeof(g_script[f]));
    g_script[f].a     = a;
    g_script[f].b     = b;
    g_script[f].start = start;
    g_script[f].back  = back;
}

int main() {
    Input_Init();

    /* 1. A pressed: fires on the very first frame it is down. */
    Frame(0, 1, 0, 0, 0);
    Input_Poll();
    CHECK((Input_GetPad(0)->pressed & PAD_A) != 0);

    /* 2. A held: must NOT fire again on the next frame (the regression). */
    Frame(1, 1, 0, 0, 0);
    Input_Poll();
    CHECK((Input_GetPad(0)->pressed & PAD_A) == 0);
    CHECK((Input_GetPad(0)->buttons & PAD_A) != 0);

    /* 3. A released: fires `released` exactly once. */
    Frame(2, 0, 0, 0, 0);
    Input_Poll();
    CHECK((Input_GetPad(0)->released & PAD_A) != 0);
    CHECK((Input_GetPad(0)->pressed & PAD_A) == 0);

    /* 4. Locked-out frame: nothing fires while nothing changes. */
    Frame(3, 0, 0, 0, 0);
    Input_Poll();
    CHECK((Input_GetPad(0)->pressed & PAD_A) == 0);
    CHECK((Input_GetPad(0)->released & PAD_A) == 0);

    /* 5. B press + hold: B/Back must fire once, then stay quiet. */
    Frame(4, 0, 1, 0, 0);
    Input_Poll();
    CHECK((Input_GetPad(0)->pressed & PAD_B) != 0);
    Frame(5, 0, 1, 0, 0);
    Input_Poll();
    CHECK((Input_GetPad(0)->pressed & PAD_B) == 0);

    /* 6. Start press + hold: same single-frame rule. */
    Frame(6, 0, 0, 1, 0);
    Input_Poll();
    CHECK((Input_GetPad(0)->pressed & PAD_START) != 0);
    Frame(7, 0, 0, 1, 0);
    Input_Poll();
    CHECK((Input_GetPad(0)->pressed & PAD_START) == 0);

    /* 7. Back (the controller's B label) press + hold. */
    Frame(8, 0, 0, 0, 1);
    Input_Poll();
    CHECK((Input_GetPad(0)->pressed & PAD_BACK) != 0);
    Frame(9, 0, 0, 0, 1);
    Input_Poll();
    CHECK((Input_GetPad(0)->pressed & PAD_BACK) == 0);

    if (g_fail) { std::printf("%d test(s) FAILED\n", g_fail); return 1; }
    std::printf("All input tests passed\n");
    return 0;
}
