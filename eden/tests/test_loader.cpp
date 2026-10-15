/* test_loader.cpp: host-side unit tests for the NSO/NRO header probe.
 *
 * Runs on the CI host (plain g++) to prove the parser is bounds-checked and
 * honest. No libXenon and no console are needed. Build:
 *   g++ -std=c++11 -Wall -I eden/source/core \
 *       eden/tests/test_loader.cpp eden/source/core/loader.cpp -o /tmp/t && /tmp/t
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "loader.h"

static int g_fail = 0;

#define CHECK(cond) \
    do { if (!(cond)) { \
        std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        ++g_fail; } } while (0)

static void Set32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* Builds a well-formed NSO header with segments inside a 0x500-byte buffer. */
static uint32_t MakeNso(unsigned char *b, int in_bounds) {
    std::memset(b, 0, 0x500);
    b[0] = 'N'; b[1] = 'S'; b[2] = 'O'; b[3] = '0';
    Set32(b + 0x04, 1);          /* version */
    Set32(b + 0x0C, 2);          /* flags    */
    Set32(b + 0x10, 0x100);      /* text off */
    Set32(b + 0x14, in_bounds ? 0x200 : 0xFFFFFFu); /* text size */
    Set32(b + 0x20, 0x300);      /* ro off   */
    Set32(b + 0x24, 0x100);      /* ro size  */
    Set32(b + 0x28, 0x400);      /* data off */
    Set32(b + 0x2C, 0x80);       /* data size*/
    Set32(b + 0x30, 0x40);       /* bss      */
    for (int i = 0; i < 32; ++i) b[0x38 + i] = (unsigned char)(0xA0 + i);
    return 0x500;
}

static uint32_t MakeNro(unsigned char *b, uint32_t sz) {
    std::memset(b, 0, 0x20);
    b[0] = 'N'; b[1] = 'R'; b[2] = 'O'; b[3] = '0';
    Set32(b + 0x04, 2);          /* version */
    Set32(b + 0x08, sz);         /* NroSize */
    Set32(b + 0x0C, 1);          /* flags   */
    return 0x20;
}

int main() {
    static unsigned char buf[0x500];
    ModuleInfo m;

    /* 1. well-formed NSO: recognised, segments and build id decoded. */
    {
        uint32_t len = MakeNso(buf, 1);
        CHECK(Loader_ProbeModule(buf, len, &m) == 1);
        CHECK(m.valid == 1);
        CHECK(m.is_nso == 1);
        CHECK(m.version == 1);
        CHECK(m.flags == 2);
        CHECK(m.has_segments == 1);
        CHECK(m.text_off == 0x100 && m.text_size == 0x200);
        CHECK(m.ro_off == 0x300 && m.ro_size == 0x100);
        CHECK(m.data_off == 0x400 && m.data_size == 0x80);
        CHECK(m.bss_size == 0x40);
        CHECK(m.has_build_id == 1);
        CHECK(m.build_id[0] == 0xA0 && m.build_id[31] == 0xBF);
    }

    /* 2. out-of-bounds segments: still an NSO header, but no trusted layout. */
    {
        uint32_t len = MakeNso(buf, 0);
        CHECK(Loader_ProbeModule(buf, len, &m) == 1);
        CHECK(m.valid == 1);
        CHECK(m.is_nso == 1);
        CHECK(m.has_segments == 0);   /* 0x100 + 0xFFFFFF overruns the buffer */
    }

    /* 3. truncated NSO (shorter than the header we need) is rejected. */
    {
        MakeNso(buf, 1);
        CHECK(Loader_ProbeModule(buf, 0x40, &m) == 0);
        CHECK(m.valid == 0);
    }

    /* 4. NRO: magic/version/size/flags only, no segment trust. */
    {
        uint32_t len = MakeNro(buf, 0x1000);
        CHECK(Loader_ProbeModule(buf, len, &m) == 1);
        CHECK(m.valid == 1);
        CHECK(m.is_nso == 0);
        CHECK(m.version == 2);
        CHECK(m.declared_size == 0x1000);
        CHECK(m.has_segments == 0);
        CHECK(m.has_build_id == 0);
    }

    /* 5. not a module: rejected, and `out` cleared. */
    {
        std::memset(buf, 'X', sizeof(buf));
        CHECK(Loader_ProbeModule(buf, sizeof(buf), &m) == 0);
        CHECK(m.valid == 0);
        CHECK(m.is_nso == 0);
    }

    /* 6. null / empty inputs are safe. */
    {
        CHECK(Loader_ProbeModule(0, 16, &m) == 0);
        CHECK(Loader_ProbeModule(buf, 0, &m) == 0);
        CHECK(Loader_ProbeModule(buf, 3, &m) == 0);
    }

    if (g_fail) { std::printf("%d test(s) FAILED\n", g_fail); return 1; }
    std::printf("All loader tests passed\n");
    return 0;
}
