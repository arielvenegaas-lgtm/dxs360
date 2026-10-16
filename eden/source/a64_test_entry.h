/* a64_test_entry.h: deterministic AArch64 interpreter self-test entry point.
 *
 * Wired into dxs360 as the first integration point for emucore/a64. It is a
 * plain C function so both the host test harness and the bare-metal libXenon
 * build can call it. Nothing in the shipped frontend calls it yet.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Run a fixed AArch64 program built from known instruction encodings and
 * verify registers, flags (where implemented), memory loads/stores, branches
 * and the final PC. Returns 0 if every check passed, otherwise the number of
 * failed checks. */
int eden_a64_selftest(void);

/* Human-readable, comma-separated list of failed check names from the last
 * run. Empty string when the last run passed. */
const char *eden_a64_selftest_report(void);

#ifdef __cplusplus
}
#endif
