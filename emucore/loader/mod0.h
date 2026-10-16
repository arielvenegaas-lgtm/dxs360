/* mod0.h: MOD0 module-header parsing + NSO dynamic relocations.
 *
 * A retail NSO's .text begins with a small module header (a MOD0 pointer and
 * the MOD0 structure) followed by code. MOD0 carries the offset of the ELF
 * .dynamic section, which in turn describes the RELA relocation table and the
 * dynamic symbol table. Applying those relocations is mandatory before
 * execution: without them the module's GOT/function pointers still hold link
 * addresses and the very first call through one faults.
 *
 * This module implements real relocation processing (no skipping of faults):
 *   - R_AARCH64_RELATIVE: write base + addend (the bulk of a module).
 *   - Symbol relocations (ABS64 / GLOB_DAT / JUMP_SLOT): resolve the symbol.
 *     Defined symbols map to base + st_value; undefined symbols are imports
 *     and are given a per-symbol stub address in a reserved guest region so the
 *     GOT entry is a mapped pointer rather than null. Function imports get an
 *     `hlt #idx` trap; object imports get a zeroed slot. On an A64_HALT_HLT the
 *     immediate names the import that was reached, so a missing SDK symbol is
 *     reported precisely instead of faulting on a null dereference.
 *
 * Pure C, no libc, exercised by the host tests, the big-endian PPC test and the
 * bare-metal build.
 */
#pragma once

#include <stdint.h>
#include "a64_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOD0_MAX_IMPORTS 1024
#define MOD0_NAME_MAX    96

/* AArch64 relocation types we handle. */
#define R_AARCH64_ABS64      257
#define R_AARCH64_GLOB_DAT   1025
#define R_AARCH64_JUMP_SLOT  1026
#define R_AARCH64_RELATIVE   1027

typedef struct Mod0Import {
    char     name[MOD0_NAME_MAX];
    uint64_t stub_addr;  /* guest address relocations point at for this symbol */
    uint8_t  st_info;    /* ELF symbol info (bind/type)                       */
    int      is_func;    /* 1 = code stub (hlt), 0 = data stub (zeros)         */
} Mod0Import;

typedef struct Mod0Info {
    int      present;        /* 1 if a MOD0 header was found                   */
    uint64_t base;           /* module load base (text location)               */
    uint64_t module_offset;  /* MOD0 magic offset from the module base          */
    uint64_t dynamic;        /* ELF .dynamic guest address (== mod0 + field)    */

    /* Relocation / symbol tables (0 if absent). */
    uint64_t rela, relasz, relaent;          /* DT_RELA table               */
    uint64_t jmprel, jmprelsz, pltrel;       /* DT_JMPREL (PLT) table       */
    uint64_t symtab, strtab, strsz, syment;
    uint64_t init, fini, init_array, init_arraysz;

    /* Import-stub region (filled by Mod0_Relocate). */
    uint64_t stub_base, stub_used;

    Mod0Import imports[MOD0_MAX_IMPORTS];
    int        import_count;

    /* Statistics for honest reporting. */
    uint64_t rel_total;       /* RELA entries seen (both tables)               */
    uint64_t rel_relative;    /* R_AARCH64_RELATIVE applied                     */
    uint64_t rel_jmprel;      /* entries from the DT_JMPREL (PLT) table         */
    uint64_t sym_total;       /* symbol relocations seen                       */
    uint64_t sym_defined;     /* resolved to a defined symbol in this module   */
    uint64_t sym_imports;     /* resolved to an import stub                    */
    uint64_t sym_unresolved;  /* could not be classified (counted, not skipped)*/
} Mod0Info;

/* Locate the MOD0 header in loaded `text` and parse the ELF .dynamic section.
 * Discovery follows the verified module-independent rule (the u32 at text+4 is
 * the offset from the module base to the MOD0 magic); MOD0 field offsets are
 * then resolved relative to the MOD0 structure address. Returns 1 if a usable
 * MOD0 was found. */
int Mod0_Locate(A64GuestMem *mem, uint64_t text_base, Mod0Info *out);

/* Apply relocations into guest memory. `base` is the load bias (0 for a module
 * linked at 0). `stub_base`/`stub_cap` describe the reserved guest region that
 * import stubs are written into. Returns 1 on success; on a malformed table it
 * returns 0 having applied what it could and left the counters reflecting it. */
int Mod0_Relocate(A64GuestMem *mem, Mod0Info *info, uint64_t base,
                  uint64_t stub_base, uint64_t stub_cap);

/* If `addr` falls inside the import-stub region, return that import's name;
 * otherwise NULL. Used to name an A64_HALT_HLT. */
const char *Mod0_ImportNameFor(const Mod0Info *info, uint64_t addr);
