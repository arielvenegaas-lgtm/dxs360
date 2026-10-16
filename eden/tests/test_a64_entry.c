/* Host-side driver for the deterministic AArch64 interpreter self-test.
 *
 * Build (from the repository root):
 *   gcc -std=c99 -Wall -Wextra -I emucore/a64 -c emucore/a64/a64.c -o a64.o
 *   gcc -std=c99 -Wall -Wextra -I emucore/a64 \
 *       eden/tests/test_a64_entry.c eden/source/a64_test_entry.c a64.o \
 *       -o test_a64_entry && ./test_a64_entry
 */
#include <stdio.h>

#include "a64_test_entry.h"

int main(void) {
    int fails = eden_a64_selftest();
    if (fails == 0) {
        printf("PASS: a64 deterministic self-test "
               "(registers, flags, memory, branches, final PC)\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed: %s\n", fails, eden_a64_selftest_report());
    return 1;
}
