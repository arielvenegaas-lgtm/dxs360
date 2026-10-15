/* nav.cpp: see nav.h. */
#include "nav.h"

#define STICK_THRESH 0.5f
#define REPEAT_DELAY 0.35f
#define REPEAT_RATE  0.11f

typedef struct { int prev; float t; } Repeater;

static Repeater r_up, r_down, r_left, r_right;

static int Repeat(int held, Repeater *r, float dt) {
    if (!held) { r->prev = 0; r->t = 0.0f; return 0; }
    if (!r->prev) { r->prev = 1; r->t = REPEAT_DELAY; return 1; }
    r->t -= dt;
    if (r->t <= 0.0f) { r->t += REPEAT_RATE; return 1; }
    return 0;
}

void Nav_Reset(void) {
    r_up.prev = r_down.prev = r_left.prev = r_right.prev = 0;
    r_up.t = r_down.t = r_left.t = r_right.t = 0.0f;
}

void Nav_Update(const PadState *pad, float dt, Nav *out) {
    if (!out) return;
    out->up = out->down = out->left = out->right = 0;
    out->accept = out->back = out->start = out->lb = out->rb = 0;
    if (!pad) return;

    int up    = (pad->buttons & PAD_UP)    || pad->ly >  STICK_THRESH;
    int down  = (pad->buttons & PAD_DOWN)  || pad->ly < -STICK_THRESH;
    int left  = (pad->buttons & PAD_LEFT)  || pad->lx < -STICK_THRESH;
    int right = (pad->buttons & PAD_RIGHT) || pad->lx >  STICK_THRESH;

    out->up    = Repeat(up,    &r_up,    dt);
    out->down  = Repeat(down,  &r_down,  dt);
    out->left  = Repeat(left,  &r_left,  dt);
    out->right = Repeat(right, &r_right, dt);

    out->accept = (pad->pressed & PAD_A)     ? 1 : 0;
    out->back   = (pad->pressed & PAD_B)     ? 1 : 0;
    out->start  = (pad->pressed & PAD_START) ? 1 : 0;
    out->lb     = (pad->pressed & PAD_LB)    ? 1 : 0;
    out->rb     = (pad->pressed & PAD_RB)    ? 1 : 0;
}
