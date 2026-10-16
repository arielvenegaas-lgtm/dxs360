/* Host-side test for the eden emucore bridge (eden/source/core/emucore.c).
 *
 * Build (from the repository root):
 *   gcc -std=c99 -Wall -Wextra -O1 \
 *       -I emucore/a64 -I emucore/hle -I emucore/loader -I eden/source -I eden/source/core \
 *       eden/tests/test_emucore.c eden/source/core/emucore.c eden/source/a64_test_entry.c \
 *       emucore/a64/a64.c emucore/a64/a64_mem.c emucore/hle/hle.c \
 *       emucore/loader/pfs0.c emucore/loader/nso.c emucore/loader/loader.c \
 *       -o /tmp/test_emucore && /tmp/test_emucore
 */
#include <stdio.h>
#include <string.h>

#include "emucore.h"

#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++fails; } } while (0)

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

/* NSO with: text (raw, at 0x1000), rodata (lz4 "WXYZ", at 0x2000),
 * data (raw 8 bytes + 16 bss, at 0x3000). Entry = 0x1000. */
static uint64_t build_nso(uint8_t *buf) {
    const uint8_t rodata_compressed[5] = { 0x40, 'W', 'X', 'Y', 'Z' };
    const uint8_t data_raw[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint32_t text_off = 0x100, ro_off = 0x108, data_off = 0x10D;

    memset(buf, 0, data_off + 8);
    memcpy(buf, "NSO0", 4);
    put32(buf + 0x0C, 1u << 1); /* flags: rodata compressed */

    put32(buf + 0x10, text_off); put32(buf + 0x14, 0x00001000u); put32(buf + 0x18, 8);  put32(buf + 0x1C, 0);
    put32(buf + 0x20, ro_off);   put32(buf + 0x24, 0x00002000u); put32(buf + 0x28, 4);  put32(buf + 0x2C, 0);
    put32(buf + 0x30, data_off); put32(buf + 0x34, 0x00003000u); put32(buf + 0x38, 8);  put32(buf + 0x3C, 16);
    put32(buf + 0x64, 5); /* rodata compressed size */

    put32(buf + text_off + 0, 0xD2824681u); /* movz x1, #0x1234 */
    put32(buf + text_off + 4, 0xD4200000u); /* brk #0 */
    memcpy(buf + ro_off, rodata_compressed, 5);
    memcpy(buf + data_off, data_raw, 8);
    return data_off + 8;
}

/* PFS0 with one entry whose data lies beyond a small header buffer. */
static uint64_t build_pfs0_header(uint8_t *buf) {
    const char *name = "x.nca";
    uint32_t strtab = 6;
    uint64_t header = 0x10 + 1 * 0x18 + strtab;

    memset(buf, 0, (size_t)header);
    memcpy(buf, "PFS0", 4);
    put32(buf + 4, 1);
    put32(buf + 8, strtab);
    put64(buf + 0x10, header);      /* absolute offset */
    put64(buf + 0x18, 0x100000u);   /* entry size, far past the header buffer */
    put32(buf + 0x20, 0);
    memcpy(buf + header - strtab, name, 6);
    return header;
}

int main(void) {
    int fails = 0;

    CHECK(EmuCore_Init() == 1, "init ok");
    CHECK(EmuCore_Active() == 1, "active after init");
    CHECK(EmuCore_BackendName() && EmuCore_BackendName()[0], "backend name");
    CHECK(EmuCore_StatusLine() && EmuCore_StatusLine()[0], "status line");

    /* Deterministic interpreter self-test must pass through the bridge. */
    int self = EmuCore_SelfTest();
    if (self != 0) printf("FAIL: self-test reported %d: %s\n", self, EmuCore_SelfTestReport());
    fails += (self != 0);

    /* Load + run a synthetic NSO. */
    {
        uint8_t nso[512];
        uint64_t sz = build_nso(nso);
        CHECK(EmuCore_LoadNso(nso, (unsigned long)sz) == 1, "nso loads");
        CHECK(EmuCore_Entry() == 0x1000u, "entry point");
        int stopped = EmuCore_RunSlice(1000u);
        CHECK(stopped == 1, "slice stops on brk");
        CHECK(strcmp(EmuCore_HaltName(), "brk") == 0, "halt reason is brk");
        CHECK(EmuCore_InstrCount() == 1, "one instruction retired before brk");
    }

    /* Bad NSO must fail, not pretend. */
    {
        uint8_t bad[16];
        memset(bad, 0, sizeof(bad));
        CHECK(EmuCore_LoadNso(bad, sizeof(bad)) == 0, "bad nso rejected");
    }

    /* PFS0 probe over a partial header buffer (data beyond the buffer). */
    {
        uint8_t hdr[128];
        uint64_t header = build_pfs0_header(hdr);
        uint64_t big = header + 0x100000u; /* real file length covers the entry */
        const char *reason = 0;
        int cnt = EmuCore_ProbePfs0(hdr, (unsigned long)header, big, &reason);
        CHECK(cnt == 1, "pfs0 probe counts one entry");
        cnt = EmuCore_ProbePfs0("nope", 4, 4, &reason);
        CHECK(cnt < 0 && reason, "pfs0 probe rejects bad magic");
    }

    if (fails == 0) {
        printf("PASS: emucore bridge (init, self-test, load NSO, run slice, PFS0 probe)\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", fails);
    return 1;
}
