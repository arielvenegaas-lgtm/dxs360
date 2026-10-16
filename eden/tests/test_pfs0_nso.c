/* Host-side test for the portable PFS0 + NSO loaders.
 *
 * Build (from the repository root):
 *   gcc -std=c99 -Wall -Wextra -I emucore/a64 -I emucore/loader \
 *       eden/tests/test_pfs0_nso.c emucore/a64/a64.c emucore/a64/a64_mem.c \
 *       emucore/loader/pfs0.c emucore/loader/nso.c emucore/loader/loader.c \
 *       -o test_pfs0_nso && ./test_pfs0_nso
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "a64.h"
#include "a64_mem.h"
#include "pfs0.h"
#include "nso.h"
#include "loader.h"

#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++fails; } } while (0)

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32));
}

/* Build a 2-file PFS0. If `relative`, store offsets relative to the header. */
static uint64_t build_pfs0(uint8_t *buf, int relative) {
    const char *d0 = "HELLO", *d1 = "WORLD!";
    uint32_t strtab = 6 + 8;               /* "a.bin\0" + "b/c.dat\0" */
    uint64_t header = 0x10 + 2 * 0x18 + strtab;

    memset(buf, 0, (size_t)(header + 11));
    memcpy(buf, "PFS0", 4);
    put32(buf + 4, 2);
    put32(buf + 8, strtab);
    put32(buf + 12, 0);

    put64(buf + 0x10, relative ? 0 : header); /* entry 0 offset */
    put64(buf + 0x18, 5);
    put32(buf + 0x20, 0);                     /* name_off -> "a.bin" */

    put64(buf + 0x28, relative ? (uint64_t)(header - header) + 5 : header + 5);
    put64(buf + 0x30, 6);
    put32(buf + 0x38, 6);                     /* name_off -> "b/c.dat" */

    memcpy(buf + header - strtab, "a.bin", 5); buf[header - strtab + 5] = 0;
    memcpy(buf + header - strtab + 6, "b/c.dat", 7); buf[header - strtab + 13] = 0;

    memcpy(buf + header + 0, d0, 5);
    memcpy(buf + header + 5, d1, 6);
    return header + 5 + 6;
}

static uint64_t build_nso(uint8_t *buf) {
    const uint8_t rodata_compressed[5] = { 0x40, 'W', 'X', 'Y', 'Z' }; /* lz4: literals only */
    const uint8_t data_raw[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint32_t text_off = 0x100;
    uint32_t ro_off = text_off + 8;
    uint32_t data_off = ro_off + 5;

    memset(buf, 0, data_off + 8);
    memcpy(buf, "NSO0", 4);
    put32(buf + 4, 0);              /* version */
    put32(buf + 0x0C, 1u << 1);     /* flags: rodata compressed */

    /* text (seg 0) */
    put32(buf + 0x10, text_off); put32(buf + 0x14, 0x10000000u);
    put32(buf + 0x18, 8); put32(buf + 0x1C, 0);
    /* rodata (seg 1) */
    put32(buf + 0x20, ro_off); put32(buf + 0x24, 0x10001000u);
    put32(buf + 0x28, 4); put32(buf + 0x2C, 0);
    /* data (seg 2) */
    put32(buf + 0x30, data_off); put32(buf + 0x34, 0x10002000u);
    put32(buf + 0x38, 8); put32(buf + 0x3C, 16); /* bss_size */

    put32(buf + 0x60, 0);   /* text compressed size (n/a) */
    put32(buf + 0x64, 5);   /* rodata compressed size */
    put32(buf + 0x68, 8);   /* data compressed size (n/a) */

    put32(buf + text_off + 0, 0xD2824681u); /* movz x1, #0x1234 */
    put32(buf + text_off + 4, 0xD4200000u); /* brk #0 */
    memcpy(buf + ro_off, rodata_compressed, 5);
    memcpy(buf + data_off, data_raw, 8);
    return data_off + 8;
}

int main(int argc, char **argv) {
    int fails = 0;

    /* ---- LZ4 ---- */
    {
        uint8_t out[16]; uint32_t n = 0;
        const uint8_t lit[5] = { 0x40, 'A', 'B', 'C', 'D' };
        const uint8_t back[7] = { 0x40, 'A', 'B', 'C', 'D', 0x04, 0x00 };
        const uint8_t bad[3] = { 0x00, 0x00, 0x00 };
        CHECK(Nso_Lz4Decompress(lit, 5, out, sizeof(out), &n) && n == 4 && memcmp(out, "ABCD", 4) == 0, "lz4 literals");
        CHECK(Nso_Lz4Decompress(back, 7, out, sizeof(out), &n) && n == 8 && memcmp(out, "ABCDABCD", 8) == 0, "lz4 backref");
        CHECK(Nso_Lz4Decompress(bad, 3, out, sizeof(out), &n) == 0, "lz4 rejects offset 0");
        CHECK(Nso_Lz4Decompress(lit, 5, out, 3, &n) == 0, "lz4 rejects overflow");
    }

    /* ---- PFS0 (absolute) ---- */
    {
        uint8_t buf[512];
        uint64_t sz = build_pfs0(buf, 0);
        Pfs0 p;
        CHECK(Pfs0_Parse(&p, buf, sz) == PFS0_OK, "pfs0 abs parses");
        CHECK(p.count == 2 && p.relative == 0, "pfs0 abs count/relative");
        CHECK(strcmp(Pfs0_Name(&p, 0), "a.bin") == 0, "pfs0 name 0");
        CHECK(strcmp(Pfs0_Name(&p, 1), "b/c.dat") == 0, "pfs0 name 1");
        CHECK(Pfs0_Find(&p, "b/c.dat") == 1, "pfs0 find");
        {
            uint64_t s = 0; const uint8_t *d = Pfs0_Data(&p, 1, &s);
            CHECK(d && s == 6 && memcmp(d, "WORLD!", 6) == 0, "pfs0 data 1");
        }
    }

    /* ---- PFS0 (header-relative, like the shipped NSP) ---- */
    {
        uint8_t buf[512];
        uint64_t sz = build_pfs0(buf, 1);
        Pfs0 p;
        CHECK(Pfs0_Parse(&p, buf, sz) == PFS0_OK, "pfs0 rel parses");
        CHECK(p.relative == 1, "pfs0 rel detected");
        {
            uint64_t s = 0; const uint8_t *d = Pfs0_Data(&p, 0, &s);
            CHECK(d && s == 5 && memcmp(d, "HELLO", 5) == 0, "pfs0 rel data 0");
        }
    }

    /* ---- PFS0 errors ---- */
    {
        uint8_t buf[64];
        Pfs0 p;
        memset(buf, 0, sizeof(buf));
        CHECK(Pfs0_Parse(&p, buf, sizeof(buf)) == PFS0_ERR_BAD_MAGIC, "pfs0 bad magic");
        buf[0] = 'P'; buf[1] = 'F'; buf[2] = 'S'; buf[3] = '0';
        CHECK(Pfs0_Parse(&p, buf, 8) == PFS0_ERR_TOO_SMALL, "pfs0 too small");
        put32(buf + 4, 999);
        CHECK(Pfs0_Parse(&p, buf, sizeof(buf)) == PFS0_ERR_TOO_MANY, "pfs0 too many");
    }

    /* ---- NSO parse + load into guest memory + execute ---- */
    {
        uint8_t nso[512];
        uint64_t nso_size = build_nso(nso);
        Nso n;
        uint8_t gmem[0x4000];
        A64GuestMem mem;
        A64 cpu;
        LoadedImage img;
        const uint8_t *d;

        CHECK(Nso_Parse(&n, nso, nso_size) == NSO_OK, "nso parses");
        CHECK(n.entry == 0x10000000u, "nso entry = text location");
        CHECK(n.seg[1].compressed && n.seg[1].compressed_size == 5, "nso rodata compressed");
        CHECK(n.seg[2].bss_size == 16, "nso data bss");

        A64GuestMem_Init(&mem, gmem, sizeof(gmem), 0x10000000u);
        CHECK(Nso_LoadIntoGuest(&n, nso, nso_size, &mem, &img) == LOAD_OK, "nso loads");
        CHECK(img.entry == 0x10000000u, "loaded entry");
        CHECK(img.image_base == 0x10000000u, "image base");
        CHECK(img.image_end == 0x10002018u, "image end includes bss");

        d = A64GuestMem_Host(&mem, 0x10000000u, 8);
        CHECK(d && d[0] == 0x81 && d[1] == 0x46 && d[2] == 0x82 && d[3] == 0xD2, "text bytes loaded");
        d = A64GuestMem_Host(&mem, 0x10001000u, 4);
        CHECK(d && memcmp(d, "WXYZ", 4) == 0, "rodata decompressed");
        d = A64GuestMem_Host(&mem, 0x10002000u, 8);
        CHECK(d && d[0] == 1 && d[7] == 8, "data bytes loaded");
        d = A64GuestMem_Host(&mem, 0x10002008u, 16);
        CHECK(d && d[0] == 0 && d[15] == 0, "bss zeroed");

        A64GuestMem_Attach(&mem, &cpu);
        A64_Reset(&cpu, img.entry, 1);
        CHECK(A64_Step(&cpu) == A64_OK, "loaded code step 1");
        CHECK(cpu.x[1] == 0x1234u, "loaded code computed x1");
        CHECK(A64_Step(&cpu) == A64_HALT_BRK, "loaded code reaches brk");
    }

    /* ---- Optional: parse a real NSP passed on the command line ----
     * Skipped in CI where no NSP is present. This validates the parser
     * against real container bytes; it does not decrypt any NCA. */
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) {
            printf("NOTE: could not open %s (skipping NSP parse)\n", argv[1]);
        } else {
            long end;
            uint8_t *nsp;
            Pfs0 p;
            uint32_t i;
            size_t got;

            fseek(f, 0, SEEK_END);
            end = ftell(f);
            rewind(f);
            nsp = (end > 0) ? (uint8_t *)malloc((size_t)end) : NULL;
            got = nsp ? fread(nsp, 1, (size_t)end, f) : 0;
            fclose(f);

            if (nsp && got == (size_t)end) {
                Pfs0Result r = Pfs0_Parse(&p, nsp, (uint64_t)end);
                CHECK(r == PFS0_OK, "real NSP parses as PFS0");
                if (r == PFS0_OK) {
                    printf("NSP: %s\n", argv[1]);
                    printf("  size=%lld files=%u relative_offsets=%d\n",
                           (long long)end, p.count, p.relative);
                    for (i = 0; i < p.count; ++i) {
                        uint64_t s = 0;
                        const uint8_t *e = Pfs0_Data(&p, i, &s);
                        CHECK(e != NULL, "real NSP entry data in range");
                        printf("  [%u] %-48s off=0x%llx size=0x%llx\n",
                               i, Pfs0_Name(&p, i),
                               (unsigned long long)p.entries[i].offset,
                               (unsigned long long)s);
                    }
                }
            } else {
                printf("NOTE: could not read %s (skipping NSP parse)\n", argv[1]);
            }
            free(nsp);
        }
    }

    if (fails == 0) {
        printf("PASS: pfs0_nso (parse, layout auto-detect, lz4, load + execute)\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", fails);
    return 1;
}
