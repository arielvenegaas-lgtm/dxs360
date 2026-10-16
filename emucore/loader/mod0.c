/* mod0.c: MOD0 parsing + NSO dynamic relocations (see mod0.h). */
#include "mod0.h"

#include <string.h>

/* --- little guest-memory accessors --- */

static int Rd(A64GuestMem *mem, uint64_t addr, void *out, uint64_t n) {
    uint8_t *h = A64GuestMem_Host(mem, addr, n);
    if (!h) return 0;
    memcpy(out, h, (size_t)n);
    return 1;
}
static int Wr(A64GuestMem *mem, uint64_t addr, const void *in, uint64_t n) {
    uint8_t *h = A64GuestMem_Host(mem, addr, n);
    if (!h) return 0;
    memcpy(h, in, (size_t)n);
    return 1;
}

/* The guest is 64-bit little-endian: decode/encode explicitly so this code is
 * correct on the big-endian Xbox 360 Xenon host as well as the LE host tests. */
static int Ru32(A64GuestMem *m, uint64_t a, uint32_t *o) {
    uint8_t b[4];
    if (!Rd(m, a, b, 4)) return 0;
    *o = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 1;
}
static int Ru64(A64GuestMem *m, uint64_t a, uint64_t *o) {
    uint8_t b[8];
    if (!Rd(m, a, b, 8)) return 0;
    *o = (uint64_t)b[0] | ((uint64_t)b[1] << 8) | ((uint64_t)b[2] << 16) |
         ((uint64_t)b[3] << 24) | ((uint64_t)b[4] << 32) | ((uint64_t)b[5] << 40) |
         ((uint64_t)b[6] << 48) | ((uint64_t)b[7] << 56);
    return 1;
}
static int Wu64(A64GuestMem *m, uint64_t a, uint64_t v) {
    uint8_t b[8];
    b[0] = (uint8_t)v;         b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
    b[4] = (uint8_t)(v >> 32); b[5] = (uint8_t)(v >> 40);
    b[6] = (uint8_t)(v >> 48); b[7] = (uint8_t)(v >> 56);
    return Wr(m, a, b, 8);
}
static int Wu32(A64GuestMem *m, uint64_t a, uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
    return Wr(m, a, b, 4);
}

/* ELF dynamic tags we care about. */
#define DT_NULL         0
#define DT_PLTRELSZ     2
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_RELA         7
#define DT_RELASZ       8
#define DT_RELAENT      9
#define DT_STRSZ        10
#define DT_SYMENT       11
#define DT_INIT         12
#define DT_FINI         13
#define DT_PLTREL       20
#define DT_JMPREL       23
#define DT_INIT_ARRAY   25
#define DT_INIT_ARRAYSZ 27

#define STT_FUNC 2

/* Find "MOD0" in the module-header block at the start of .text. */
static uint64_t FindMod0(A64GuestMem *mem, uint64_t text_base) {
    uint64_t off;
    for (off = 0; off < 0x1000; off += 4) {
        uint32_t w;
        if (!Ru32(mem, text_base + off, &w)) break;
        if (w == 0x30444F4DU) return text_base + off; /* "MOD0" little-endian */
    }
    return 0;
}

int Mod0_Locate(A64GuestMem *mem, uint64_t text_base, Mod0Info *out) {
    uint64_t mod0, dyn, p, limit;
    memset(out, 0, sizeof(*out));
    out->base = text_base;

    mod0 = FindMod0(mem, text_base);
    if (!mod0) return 0;

    /* MOD0 layout: magic, dynamic, bss_start, bss_end, eh_frame_hdr, module_obj
     * (all 32-bit offsets relative to the module base). */
    {
        uint32_t dyn32;
        if (!Ru32(mem, mod0 + 4, &dyn32)) return 0;
        out->module_offset = (uint64_t)(mod0 - text_base);
        out->dynamic = text_base + dyn32;
    }
    out->present = 1;

    /* Parse the ELF .dynamic section. The MOD0 dynamic pointer addresses a
     * block that begins with a leading zero word on retail NSOs, so skip any
     * leading DT_NULL padding before the first real tag. */
    dyn = out->dynamic;
    p = dyn;
    limit = dyn + 0x400;
    for (;;) {
        uint64_t tag;
        if (!Ru64(mem, p, &tag)) return 1; /* MOD0 found; dynamic unreadable */
        if (tag != DT_NULL) break;
        p += 8;
        if (p >= limit) return 1;
    }
    for (; p < limit; p += 16) {
        uint64_t tag, val;
        if (!Ru64(mem, p, &tag) || !Ru64(mem, p + 8, &val)) break;
        switch (tag) {
        case DT_RELA:         out->rela = val; break;
        case DT_RELASZ:       out->relasz = val; break;
        case DT_RELAENT:      out->relaent = val ? val : 0x18; break;
        case DT_PLTRELSZ:     out->jmprelsz = val; break;
        case DT_PLTREL:       out->pltrel = val; break;
        case DT_JMPREL:       out->jmprel = val; break;
        case DT_SYMTAB:       out->symtab = val; break;
        case DT_STRTAB:       out->strtab = val; break;
        case DT_STRSZ:        out->strsz = val; break;
        case DT_SYMENT:       out->syment = val ? val : 0x18; break;
        case DT_INIT:         out->init = val; break;
        case DT_FINI:         out->fini = val; break;
        case DT_INIT_ARRAY:   out->init_array = val; break;
        case DT_INIT_ARRAYSZ: out->init_arraysz = val; break;
        case DT_NULL:         return 1;
        default: break;
        }
    }
    return 1;
}

/* Read a symbol's name from the dynamic string table. */
static void SymName(A64GuestMem *mem, const Mod0Info *i, uint32_t st_name,
                    char *out, int outsz) {
    int n = 0;
    out[0] = 0;
    if (!i->strtab || st_name >= i->strsz) return;
    for (;;) {
        uint8_t c;
        if (n >= outsz - 1) break;
        if (!Rd(mem, i->strtab + st_name + (uint64_t)n, &c, 1)) break;
        if (!c) break;
        out[n++] = (char)c;
    }
    out[n] = 0;
}

/* Resolve a symbol referenced by a relocation. Defined symbols map to
 * base + st_value. Undefined ones are imports: *is_import is set, the name is
 * returned and the value is 0 (the caller assigns a stub). */
static uint64_t ResolveSym(A64GuestMem *mem, Mod0Info *i, uint64_t base,
                           uint64_t symidx, int *is_import, uint8_t *out_info,
                           char *name, int nsz) {
    uint64_t ent = i->syment ? i->syment : 0x18;
    uint64_t s = i->symtab + symidx * ent;
    uint32_t st_name; uint8_t info, sh_hi, sh_lo; uint16_t shndx; uint64_t value;

    *is_import = 0; *out_info = 0; name[0] = 0;
    if (!Ru32(mem, s, &st_name)) return 0;
    if (!Rd(mem, s + 4, &info, 1)) return 0;
    *out_info = info;
    if (!Rd(mem, s + 6, &sh_lo, 1) || !Rd(mem, s + 7, &sh_hi, 1)) return 0;
    shndx = (uint16_t)(sh_lo | ((uint16_t)sh_hi << 8));
    if (!Ru64(mem, s + 8, &value)) return 0;

    SymName(mem, i, st_name, name, nsz);
    if (shndx == 0) { *is_import = 1; return 0; }
    return base + value; /* defined in this module */
}

/* Find or create an import stub for `name`. Returns its guest address. */
static uint64_t ImportStub(Mod0Info *i, const char *name, uint8_t st_info,
                           uint64_t *stub_next) {
    int k, type = st_info & 0xF;
    for (k = 0; k < i->import_count; ++k)
        if (strcmp(i->imports[k].name, name) == 0) return i->imports[k].stub_addr;
    if (i->import_count >= MOD0_MAX_IMPORTS) return 0;
    k = i->import_count++;
    memset(&i->imports[k], 0, sizeof(i->imports[k]));
    strncpy(i->imports[k].name, name, MOD0_NAME_MAX - 1);
    i->imports[k].st_info = st_info;
    i->imports[k].is_func = (type == STT_FUNC || type == 0);
    i->imports[k].stub_addr = *stub_next;
    *stub_next += 8;
    return i->imports[k].stub_addr;
}

int Mod0_Relocate(A64GuestMem *mem, Mod0Info *i, uint64_t base,
                  uint64_t stub_base, uint64_t stub_cap) {
    uint64_t ent, stub_next;
    int k, t;
    /* The two relocation tables: the main DT_RELA table and the DT_JMPREL
     * (PLT) table. Both use RELA entries here (DT_PLTREL == DT_RELA). */
    struct { uint64_t addr, size; } tabs[2];
    int ntabs = 0;

    if (!i || !i->present) return 0;
    ent = i->relaent ? i->relaent : 0x18;
    if (i->rela && i->relasz)            { tabs[ntabs].addr = i->rela;   tabs[ntabs++].size = i->relasz; }
    if (i->jmprel && i->jmprelsz)        { tabs[ntabs].addr = i->jmprel; tabs[ntabs++].size = i->jmprelsz; }
    if (!ntabs) return 1; /* nothing to relocate */

    i->stub_base = stub_base;
    stub_next = stub_base;

    /* Pass 1: classify every relocation and assign stubs to imports. */
    for (t = 0; t < ntabs; ++t) {
        uint64_t n = tabs[t].size / ent, r;
        if (t == 1) i->rel_jmprel += n;
        i->rel_total += n;
        for (r = 0; r < n; ++r) {
            uint64_t eoff = tabs[t].addr + r * ent;
            uint64_t off, info, symidx; uint32_t type;
            char name[MOD0_NAME_MAX]; uint8_t sinfo; int is_import = 0;
            if (!Ru64(mem, eoff, &off)) break;
            if (!Ru64(mem, eoff + 8, &info)) break;
            type = (uint32_t)(info & 0xFFFFFFFFu);
            symidx = info >> 32;
            if (type == R_AARCH64_RELATIVE) { i->rel_relative++; continue; }
            if (type == R_AARCH64_ABS64 || type == R_AARCH64_GLOB_DAT ||
                type == R_AARCH64_JUMP_SLOT) {
                i->sym_total++;
                ResolveSym(mem, i, base, symidx, &is_import, &sinfo, name, sizeof(name));
                if (is_import && !ImportStub(i, name, sinfo, &stub_next))
                    i->sym_unresolved++;
            } else {
                i->sym_unresolved++; /* unknown type: counted, not skipped */
            }
        }
    }
    i->stub_used = stub_next > stub_base ? (stub_next - stub_base) : 0;
    if (i->stub_used > stub_cap) return 0;

    /* Materialise the stub region. Function imports trap with `hlt #idx` so a
     * call through an unresolved SDK import halts with a nameable immediate;
     * object imports get a zeroed slot. */
    for (k = 0; k < i->import_count; ++k) {
        Mod0Import *im = &i->imports[k];
        uint8_t zeros[8] = {0};
        if (im->is_func) {
            uint32_t hlt = 0xD4400000u | ((uint32_t)k << 5); /* hlt #k */
            if (!Wu32(mem, im->stub_addr, hlt)) return 0;
        } else {
            if (!Wr(mem, im->stub_addr, zeros, 8)) return 0;
        }
    }

    /* Pass 2: apply. */
    for (t = 0; t < ntabs; ++t) {
        uint64_t n = tabs[t].size / ent, r;
        for (r = 0; r < n; ++r) {
            uint64_t eoff = tabs[t].addr + r * ent;
            uint64_t off, info, addend, symidx, value; uint32_t type;
            char name[MOD0_NAME_MAX]; uint8_t sinfo; int is_import = 0;
            if (!Ru64(mem, eoff, &off)) break;
            if (!Ru64(mem, eoff + 8, &info)) break;
            if (!Ru64(mem, eoff + 16, &addend)) break;
            type = (uint32_t)(info & 0xFFFFFFFFu);
            symidx = info >> 32;

            if (type == R_AARCH64_RELATIVE) {
                if (!Wu64(mem, base + off, base + addend)) return 0;
            } else if (type == R_AARCH64_ABS64 || type == R_AARCH64_GLOB_DAT ||
                       type == R_AARCH64_JUMP_SLOT) {
                value = ResolveSym(mem, i, base, symidx, &is_import, &sinfo,
                                   name, sizeof(name));
                if (!is_import) {
                    i->sym_defined++;
                } else {
                    value = 0;
                    for (k = 0; k < i->import_count; ++k)
                        if (strcmp(i->imports[k].name, name) == 0) {
                            value = i->imports[k].stub_addr; break;
                        }
                    if (!value) { i->sym_unresolved++; continue; }
                    i->sym_imports++;
                }
                if (!Wu64(mem, base + off, value)) return 0;
            }
        }
    }
    return 1;
}

const char *Mod0_ImportNameFor(const Mod0Info *i, uint64_t addr) {
    int k;
    if (!i || addr < i->stub_base) return 0;
    for (k = 0; k < i->import_count; ++k)
        if (i->imports[k].stub_addr == addr) return i->imports[k].name;
    return 0;
}
