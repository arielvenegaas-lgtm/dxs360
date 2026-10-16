/* layout.h: kernel-style multi-module memory layout for an ExeFS image.
 *
 * A bootable program NCA's ExeFS holds several NSO modules (rtld, main,
 * subsdk0..subsdk9, sdk). The Horizon kernel places them at page-aligned,
 * contiguous addresses starting at the process code address, in NsoIndex order
 * (rtld first), and enters the process at the FIRST module's base -- rtld, the
 * dynamic linker, whose first instruction is `b 0x8`. rtld then maps/relocates
 * the other modules itself.
 *
 * This planner reproduces only that placement: order, page-aligned contiguous
 * bases, the per-segment absolute ranges (text/rodata/data/bss) with their
 * protections, and the entry address. It does NOT link or relocate anything --
 * that is rtld's job at runtime.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LAYOUT_MAX_MODULES 16
#define LAYOUT_PAGE        0x1000u

/* Segment protections (as the kernel maps them). The flat guest-RAM model does
 * not yet trap these; they are carried so callers can report exactly how a
 * module is meant to be mapped. */
#define LAYOUT_PROT_READ  1u
#define LAYOUT_PROT_WRITE 2u
#define LAYOUT_PROT_EXEC  4u

#define LAYOUT_SEG_TEXT 0
#define LAYOUT_SEG_RO   1
#define LAYOUT_SEG_DATA 2
#define LAYOUT_SEG_BSS  3

typedef struct LayoutSegment {
    uint64_t addr;   /* absolute guest address */
    uint64_t size;   /* bytes */
    uint32_t prot;   /* LAYOUT_PROT_* */
} LayoutSegment;

/* One module's input: its ExeFS name and the geometry parsed from its NSO
 * header (memory offsets/sizes; bss extends the data segment). */
typedef struct LayoutInput {
    const char *name;
    uint64_t    image_size;  /* Nso_ImageSize(): unaligned mapped size */
    uint64_t    seg_loc[3];  /* text/rodata/data memory offsets          */
    uint64_t    seg_size[3]; /* text/rodata/data unaligned sizes         */
    uint64_t    bss_size;
} LayoutInput;

typedef struct LayoutModule {
    char          name[16];
    int           rank;       /* NsoIndex */
    uint64_t      base;       /* page-aligned guest load base */
    uint64_t      size;       /* page-aligned mapped size (incl. bss) */
    LayoutSegment seg[4];     /* text, rodata, data, bss (absolute) */
} LayoutModule;

typedef struct LayoutPlan {
    LayoutModule mod[LAYOUT_MAX_MODULES];
    int          count;
    uint64_t     code_address; /* base of the first module */
    uint64_t     total_size;   /* end of the last module (page-aligned) */
    uint64_t     entry;        /* code_address (the process enters rtld) */
} LayoutPlan;

/* NsoIndex rank for an ExeFS module name; -1 for anything that is not a
 * loadable NSO (e.g. "main.npdm"). A trailing ".nso" is accepted and stripped.
 *   rtld=0, main=1, wkc0..wkc9=2..11, subsdk0..subsdk9=12..21, sdk=22 */
int Layout_Rank(const char *name);

/* Build a plan from the present modules. Modules are ordered by rank, placed at
 * page-aligned contiguous bases from `code_address`, and their segments are
 * resolved to absolute ranges. Returns 1 on success; 0 if there are no loadable
 * modules, too many, a size would wrap, or the segments of a module overlap or
 * fall outside its page-aligned span (a malformed header). Nothing is written
 * on failure. */
int Layout_Plan(const LayoutInput *in, int n, uint64_t code_address,
                LayoutPlan *out);

#ifdef __cplusplus
}
#endif
