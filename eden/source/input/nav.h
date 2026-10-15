/* nav.h: turns the raw pad state into edge-triggered menu events with
 * auto-repeat for the directions. */
#pragma once

#include "input.h"

typedef struct {
    int up, down, left, right;   /* edge events, with auto-repeat */
    int accept;                  /* A pressed                     */
    int back;                    /* B pressed                     */
    int start;                   /* Start pressed                 */
    int lb, rb;                  /* bumper presses                */
} Nav;

void Nav_Reset(void);
void Nav_Update(const PadState *pad, float dt, Nav *out);
