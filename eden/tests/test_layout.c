/* test_layout.c: host tests for the kernel-style module layout planner.
 *
 * Build (from the repository root):
 *   gcc -std=c99 -Wall -Wextra -I emucore/loader \
 *       eden/tests/test_layout.c emucore/loader/layout.c -o test_layout && ./test_layout
 *
 * Verifies NsoIndex rank mapping (rtld=0, main=1, wkc/subsdk/sdk), that modules
 * are ordered rtld-first and placed at page-aligned contiguous bases, that the
 * process entry is the code address (rtld base), that "main.npdm" is excluded,
 * and that malformed geometry (overlap, out-of-range, too many) is rejected.
 * Pure C, no keys or real content.
 */
#include <stdio.h>
#include <string.h>

#include "layout.h"

#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++fails; } } while (0)

static void set_mod(LayoutInput *m, const char *name,
                    uint64_t img, uint64_t l0, uint64_t s0,
                    uint64_t l1, uint64_t s1, uint64_t l2, uint64_t s2, uint64_t bss) {
    m->name = name;
    m->image_size = img;
    m->seg_loc[0] = l0; m->seg_size[0] = s0;
    m->seg_loc[1] = l1; m->seg_size[1] = s1;
    m->seg_loc[2] = l2; m->seg_size[2] = s2;
    m->bss_size = bss;
}

int main(void) {
    int fails = 0;

    /* ---- rank mapping ---- */
    CHECK(Layout_Rank("rtld") == 0, "rtld rank 0");
    CHECK(Layout_Rank("main") == 1, "main rank 1");
    CHECK(Layout_Rank("wkc0") == 2, "wkc0 rank 2");
    CHECK(Layout_Rank("subsdk0") == 12, "subsdk0 rank 12");
    CHECK(Layout_Rank("subsdk9") == 21, "subsdk9 rank 21");
    CHECK(Layout_Rank("sdk") == 22, "sdk rank 22");
    CHECK(Layout_Rank("rtld.nso") == 0, "trailing .nso accepted");
    CHECK(Layout_Rank("main.npdm") == -1, "main.npdm is not a module");
    CHECK(Layout_Rank("bogus") == -1, "unknown name rejected");
    CHECK(Layout_Rank("subsdk10") == -1, "two-digit subsdk rejected");

    /* ---- order + page-aligned contiguous bases + entry ---- */
    {
        LayoutInput in[4];
        LayoutPlan plan;
        /* Deliberately scrambled input order. */
        set_mod(&in[0], "sdk",      0x4000, 0, 0x400, 0x1000, 0x400, 0x3000, 0x400, 0x400);
        set_mod(&in[1], "subsdk0",  0x1000, 0, 0x100, 0,      0,     0x800,  0x100, 0);
        set_mod(&in[2], "rtld",     0x6000, 0, 0x100, 0x1000, 0x100, 0x2000, 0x100, 0x100);
        set_mod(&in[3], "main",     0x3000, 0, 0x200, 0x1000, 0x200, 0x2000, 0x200, 0x100);

        CHECK(Layout_Plan(in, 4, 0, &plan) == 1, "plan builds");
        CHECK(plan.count == 4, "four modules planned");
        CHECK(strcmp(plan.mod[0].name, "rtld") == 0, "rtld first");
        CHECK(strcmp(plan.mod[1].name, "main") == 0, "main second");
        CHECK(strcmp(plan.mod[2].name, "subsdk0") == 0, "subsdk0 third");
        CHECK(strcmp(plan.mod[3].name, "sdk") == 0, "sdk last");
        CHECK(plan.entry == 0, "entry is the code address (rtld base)");
        CHECK(plan.code_address == 0, "code address");
        CHECK(plan.mod[0].base == 0x0, "rtld base 0");
        CHECK(plan.mod[1].base == 0x6000, "main base after rtld");
        CHECK(plan.mod[2].base == 0x9000, "subsdk0 base");
        CHECK(plan.mod[3].base == 0xa000, "sdk base");
        CHECK(plan.total_size == 0xe000, "total spans all page-aligned modules");
        /* Bases are contiguous and page-aligned. */
        CHECK((plan.mod[1].base % LAYOUT_PAGE) == 0, "page aligned");
        CHECK(plan.mod[1].base == plan.mod[0].base + plan.mod[0].size, "contiguous");
        /* Segment protections match the kernel mapping. */
        CHECK(plan.mod[0].seg[LAYOUT_SEG_TEXT].prot == (LAYOUT_PROT_READ | LAYOUT_PROT_EXEC),
              "text is r-x");
        CHECK(plan.mod[0].seg[LAYOUT_SEG_RO].prot == LAYOUT_PROT_READ, "rodata is r--");
        CHECK(plan.mod[0].seg[LAYOUT_SEG_DATA].prot == (LAYOUT_PROT_READ | LAYOUT_PROT_WRITE),
              "data is rw-");
        CHECK(plan.mod[0].seg[LAYOUT_SEG_BSS].prot == (LAYOUT_PROT_READ | LAYOUT_PROT_WRITE),
              "bss is rw-");
        /* Absolute segment addresses are base + in-module offset. */
        CHECK(plan.mod[1].seg[LAYOUT_SEG_TEXT].addr == 0x6000, "main text abs addr");
        CHECK(plan.mod[1].seg[LAYOUT_SEG_RO].addr == 0x7000, "main rodata abs addr");
        CHECK(plan.mod[1].seg[LAYOUT_SEG_DATA].addr == 0x8000, "main data abs addr");
        CHECK(plan.mod[1].seg[LAYOUT_SEG_BSS].addr == 0x8200, "main bss abs addr");
        CHECK(plan.mod[1].seg[LAYOUT_SEG_BSS].size == 0x100, "main bss size");
    }

    /* ---- non-modules are excluded (main.npdm) ---- */
    {
        LayoutInput in[2];
        LayoutPlan plan;
        set_mod(&in[0], "main.npdm", 0x200, 0, 0x10, 0, 0, 0, 0, 0);
        set_mod(&in[1], "rtld",      0x1000, 0, 0x100, 0, 0, 0, 0, 0);
        CHECK(Layout_Plan(in, 2, 0, &plan) == 1, "plan with npdm builds");
        CHECK(plan.count == 1, "npdm excluded");
        CHECK(strcmp(plan.mod[0].name, "rtld") == 0, "only rtld planned");
    }

    /* ---- rejections ---- */
    {
        LayoutInput in[3];
        LayoutPlan plan;
        /* No loadable modules at all. */
        set_mod(&in[0], "main.npdm", 0x200, 0, 0x10, 0, 0, 0, 0, 0);
        CHECK(Layout_Plan(in, 1, 0, &plan) == 0, "no modules rejected");

        /* Overlapping text/rodata within a module. */
        set_mod(&in[0], "rtld", 0x2000, 0, 0x200, 0x100, 0x200, 0, 0, 0);
        CHECK(Layout_Plan(in, 1, 0, &plan) == 0, "overlapping segments rejected");

        /* A segment extending past the page-aligned span. */
        set_mod(&in[0], "rtld", 0x1000, 0, 0x100, 0, 0, 0x800, 0x1000, 0);
        CHECK(Layout_Plan(in, 1, 0, &plan) == 0, "segment beyond span rejected");

        /* Too many modules. */
        {
            LayoutInput many[LAYOUT_MAX_MODULES + 1];
            static const char *names[] = { "rtld","main","wkc0","wkc1","wkc2","wkc3",
                                           "wkc4","wkc5","wkc6","wkc7","wkc8","wkc9",
                                           "subsdk0","subsdk1","subsdk2","subsdk3","sdk" };
            int k, cnt = (int)(sizeof(names) / sizeof(names[0]));
            for (k = 0; k < cnt; ++k) set_mod(&many[k], names[k], 0x1000, 0, 0x100, 0, 0, 0, 0, 0);
            CHECK(cnt > LAYOUT_MAX_MODULES, "more names than max");
            CHECK(Layout_Plan(many, cnt, 0, &plan) == 0, "too many modules rejected");
        }
    }

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("all layout checks passed\n");
    return 0;
}
