/* Host stub for libXenon's <input/input.h>.
 *
 * Only the little that xenon/source/input.cpp needs. Used by the host unit
 * tests so the real input translation unit can be compiled and exercised on a
 * plain Linux g++ build (no libXenon, no console). */
#ifndef EDEN_TEST_STUB_INPUT_H
#define EDEN_TEST_STUB_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

struct controller_data_s {
    int           a, b, x, y;
    int           up, down, left, right;
    int           start, back, lb, rb;
    short         s1_x, s1_y, s2_x, s2_y;
    unsigned char lt, rt;
};

/* Returns non-zero and fills *data when a fresh report is available. */
int get_controller_data(struct controller_data_s *data, int index);

#ifdef __cplusplus
}
#endif

#endif
