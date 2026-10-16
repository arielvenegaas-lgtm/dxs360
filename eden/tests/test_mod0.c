/* test_mod0.c: host tests for the MOD0 relocation engine and the AArch64
 * ADRP decoder that the relocations depend on.
 *
 * Build (from the repository root):
 *   gcc -std=c99 -Wall -Wextra -I emucore/a64 -I emucore/loader \
 *       eden/tests/test_mod0.c emucore/a64/a64.c emucore/a64/a64_mem.c \
 *       emucore/loader/mod0.c -o test_mod0 && ./test_mod0
 *
 * The MOD0 fixture is synthetic: a module-header block, an ELF .dynamic section
 * pointing at a DT_RELA table (one R_AARCH64_RELATIVE) and a DT_JMPREL table
 * (one R_AARCH64_JUMP_SLOT to an undefined symbol "printf"), a symbol table and
 * a string table. The test asserts the RELATIVE slot is biased by the load base,
 * the JUMP_SLOT slot receives a stub address, the stub contains the `hlt #0`
 * trap, and the import is nameable from its stub address.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a64.h"
#include "a64_mem.h"
#include "mod0.h"

#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++fails; } } while (0)

static void w32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void w64(uint8_t *p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }
static uint64_t r64(const uint8_t *p) { return (uint64_t)(uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)) | ((uint64_t)(uint32_t)(p[4] | (p[5] << 8) | (p[6] << 16) | ((uint32_t)p[7] << 24)) << 32); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

/* Build the synthetic module image. Returns 1 on success. */
static int build_module(uint8_t *img, uint64_t size) {
    memset(img, 0, (size_t)size);
    /* module header at 0: MOD0 pointer @4 -> 8, magic "MOD0" @8. */
    w32(img + 4, 8);
    memcpy(img + 8, "MOD0", 4);
    w32(img + 0x0C, 0x100);          /* MOD0.dynamic -> .dynamic at 0x100 */

    /* .dynamic at 0x100: leading zero word, then (tag,val) pairs. */
    w64(img + 0x100, 0);
    w64(img + 0x108, 5);  w64(img + 0x110, 0x400);  /* DT_STRTAB -> 0x400 */
    w64(img + 0x118, 6);  w64(img + 0x120, 0x300);  /* DT_SYMTAB -> 0x300 */
    w64(img + 0x128, 11); w64(img + 0x130, 0x18);   /* DT_SYMENT */
    w64(img + 0x138, 10); w64(img + 0x140, 0x20);   /* DT_STRSZ */
    w64(img + 0x148, 7);  w64(img + 0x150, 0x500);  /* DT_RELA -> 0x500 */
    w64(img + 0x158, 8);  w64(img + 0x160, 0x18);   /* DT_RELASZ */
    w64(img + 0x168, 9);  w64(img + 0x170, 0x18);   /* DT_RELAENT */
    w64(img + 0x178, 2);  w64(img + 0x180, 0x18);   /* DT_PLTRELSZ */
    w64(img + 0x188, 20); w64(img + 0x190, 7);      /* DT_PLTREL = RELA */
    w64(img + 0x198, 23); w64(img + 0x1A0, 0x518);  /* DT_JMPREL -> 0x518 */
    w64(img + 0x1A8, 0);  w64(img + 0x1B0, 0);      /* DT_NULL */

    /* symbol table at 0x300 (24-byte entries): [0]=null, [1]=undefined printf. */
    img[0x318] = 1;      /* st_name -> "printf" */
    img[0x31C] = 0x12;   /* st_info: GLOBAL FUNC */
    /* st_shndx = 0 -> undefined (import) */

    /* string table at 0x400: "\0printf\0". */
    memcpy(img + 0x401, "printf", 6);

    /* DT_RELA at 0x500: R_AARCH64_RELATIVE, offset 0x200, addend 0x30. */
    w64(img + 0x500, 0x200);
    w64(img + 0x508, 1027);   /* type only, symidx 0 */
    w64(img + 0x510, 0x30);

    /* DT_JMPREL at 0x518: R_AARCH64_JUMP_SLOT, offset 0x208, sym 1. */
    w64(img + 0x518, 0x208);
    w64(img + 0x520, ((uint64_t)1 << 32) | 1026);
    w64(img + 0x528, 0);
    return 1;
}

int main(void) {
    int fails = 0;

    /* ---- ADRP decode (the fix the relocations rely on) ---- */
    {
        uint8_t code[16];
        A64GuestMem mem;
        A64 cpu;
        w32(code + 0, 0xD0002660u); /* adrp x0, #0x4ce000 */
        w32(code + 4, 0xD4200000u); /* brk #0 */
        A64GuestMem_Init(&mem, code, sizeof(code), 0);
        A64_Init(&cpu, A64GuestMem_Read, A64GuestMem_Write, &mem);
        A64_Reset(&cpu, 0, 1);
        A64_Step(&cpu);
        CHECK(cpu.x[0] == 0x4ce000ULL, "adrp page base + imm<<12");
        CHECK(cpu.pc == 4, "adrp advances pc");
        /* The pre-fix decoder took the ADR path and produced 0x4ce here. */
        CHECK(cpu.x[0] != 0x4ceULL, "adrp not decoded as adr");
    }

    /* ---- MOD0 locate + relocate ---- */
    {
        uint8_t *img = (uint8_t *)malloc(0x20000);
        A64GuestMem mem;
        Mod0Info info;
        uint64_t stub_base = 0x10000, stub_cap = 0x10000;
        CHECK(build_module(img, 0x20000), "fixture builds");

        A64GuestMem_Init(&mem, img, 0x20000, 0);
        CHECK(Mod0_Locate(&mem, 0, &info) == 1, "MOD0 located");
        CHECK(info.present == 1, "MOD0 marked present");
        CHECK(info.module_offset == 8, "module_offset == 8");
        CHECK(info.dynamic == 0x100, "dynamic == 0x100");
        CHECK(info.rela == 0x500 && info.relasz == 0x18, "DT_RELA parsed");
        CHECK(info.jmprel == 0x518 && info.jmprelsz == 0x18, "DT_JMPREL parsed");
        CHECK(info.symtab == 0x300 && info.strtab == 0x400, "symbol/string tables");

        CHECK(Mod0_Relocate(&mem, &info, 0, stub_base, stub_cap) == 1, "relocate ok");
        CHECK(info.rel_total == 2, "rel_total == 2");
        CHECK(info.rel_relative == 1, "one RELATIVE");
        CHECK(info.rel_jmprel == 1, "one JMPREL");
        CHECK(info.sym_total == 1, "one symbol reloc");
        CHECK(info.sym_imports == 1, "one import resolved");
        CHECK(info.sym_unresolved == 0, "no unresolved");

        CHECK(r64(img + 0x200) == 0x30ULL, "RELATIVE wrote base+addend");
        CHECK(r64(img + 0x208) == stub_base, "JUMP_SLOT wrote stub address");
        CHECK(r32(img + stub_base) == (0xD4400000u), "stub holds hlt #0");
        CHECK(info.import_count == 1, "one import recorded");
        CHECK(strcmp(info.imports[0].name, "printf") == 0, "import name");
        CHECK(Mod0_ImportNameFor(&info, stub_base) != NULL &&
              strcmp(Mod0_ImportNameFor(&info, stub_base), "printf") == 0,
              "stub address names import");
        CHECK(Mod0_ImportNameFor(&info, stub_base + 8) == NULL, "unknown stub name");

        free(img);
    }

    /* ---- non-MOD0 image is rejected ---- */
    {
        uint8_t img[0x1000];
        A64GuestMem mem;
        Mod0Info info;
        memset(img, 0, sizeof(img));
        A64GuestMem_Init(&mem, img, sizeof(img), 0);
        CHECK(Mod0_Locate(&mem, 0, &info) == 0, "no MOD0 -> not found");
    }

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("all mod0/adrp checks passed\n");
    return 0;
}
