/* fuzz_scan.cpp: host ASan/UBSan harness for Eden's ROM-scan path.
 *
 * Exercises, under memory/UB sanitizers:
 *   1. Fat32_ListDir on a real FAT32 image with a 255-char LFN (20 LFN
 *      entries = the full `seq` range) plus scene-style names -> LfnAppend13
 *      and the directory walk.
 *   2. Library_AddFile / Library_ParseName on those names.
 *   3. Library_ScanContainer on read bytes.
 *   4. Deterministic fuzzing of PFS0/NSO/NRO headers and of ParseName input.
 *
 * Build (see .github/workflows/build-eden.yml for the CI form, which also
 * creates the FAT32 image):
 *   gcc -std=c99 -g -O1 -fsanitize=address,undefined -c xenon/source/fat32.c \
 *       -I xenon/source -o /tmp/fat32_san.o
 *   g++ -std=c++11 -Wall -g -O1 -fsanitize=address,undefined \
 *       -fno-omit-frame-pointer \
 *       -I eden/source/game -I eden/source/core -I xenon/source \
 *       fuzz_scan.cpp eden/source/game/library.cpp \
 *       eden/source/core/loader.cpp /tmp/fat32_san.o -o /tmp/fuzz_scan
 *   /tmp/fuzz_scan /tmp/roms.img
 */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>

#include "fat32.h"
#include "library.h"
#include "loader.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

/* ---- disk image reader -------------------------------------------------- */
static FILE *g_img;
static int ReadSec(void *ctx, uint32_t lba, uint32_t cnt, void *buf) {
    (void)ctx;
    if (std::fseek(g_img, (long)lba * 512, SEEK_SET)) return 0;
    return std::fread(buf, 512, cnt, g_img) == cnt;
}

/* ---- callback into the library ----------------------------------------- */
struct ScanCtx { Library lib; };
static int OnEntry(void *ctx, const char *name, uint32_t size, int is_dir) {
    ScanCtx *s = (ScanCtx *)ctx;
    if (!is_dir) Library_AddFile(&s->lib, name, size);
    return 1;
}

/* Simple deterministic PRNG (no libc rand, reproducible). */
static uint64_t g_rng = 0x123456789abcdef0ULL;
static uint32_t Rnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 32);
}
static void FillRandom(unsigned char *p, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) p[i] = (unsigned char)(Rnd() & 0xFF);
}

int main(int argc, char **argv) {
    const char *imgpath = (argc > 1) ? argv[1] : "/tmp/fuzz/roms.img";

    g_img = std::fopen(imgpath, "rb");
    if (!g_img) { std::printf("cannot open %s\n", imgpath); return 2; }

    if (!Fat32_Mount(ReadSec, 0)) { std::printf("mount FAILED\n"); return 2; }

    /* 1. Directory walk with long + scene-style names. */
    {
        ScanCtx s;
        Library_Init(&s.lib);
        long n = Fat32_ListDir("roms", OnEntry, &s);
        std::printf("ListDir roms -> %ld entries, %d games\n", n, s.lib.count);
        CHECK(n >= 4);
        for (int i = 0; i < s.lib.count; ++i) {
            const Game *g = &s.lib.items[i];
            std::printf("  [%d] file len=%zu title len=%zu title_id='%s' ver='%s' reg='%s'\n",
                        i, std::strlen(g->file), std::strlen(g->title),
                        g->title_id, g->version, g->region);
            CHECK(std::strlen(g->file) < sizeof(g->file));
            CHECK(std::strlen(g->title) < sizeof(g->title));
            CHECK(std::strlen(g->title_id) < sizeof(g->title_id));
            CHECK(std::strlen(g->version) < sizeof(g->version));
            CHECK(std::strlen(g->region) < sizeof(g->region));
            /* Every parsed field must be NUL-terminated inside its buffer. */
            for (size_t k = 0; k < sizeof(g->file); ++k) { (void)g->file[k]; }
        }
    }

    /* 2. Read each game's header exactly the way LoadGameMeta() does.
     *    LoadGameMeta sets meta_scanned up front, so a read that fails (e.g.
     *    a name longer than Game.file[] can hold) or a PFS0 shorter than
     *    0x10 must leave the struct in a sane, still-terminated state. */
    {
        ScanCtx s;
        Library_Init(&s.lib);
        Fat32_ListDir("roms", OnEntry, &s);
        static unsigned char buf[1 << 16];
        for (int i = 0; i < s.lib.count; ++i) {
            Game *g = &s.lib.items[i];
            g->meta_scanned = 1;
            char path[512];
            std::snprintf(path, sizeof(path), "roms/%s", g->file);
            long r = Fat32_ReadFile(path, buf, (uint32_t)sizeof(buf));
            if (r > 0) Library_ScanContainer(buf, (uint32_t)r, g);
            CHECK(g->meta_scanned == 1);
            CHECK(std::memchr(g->title_id, 0, sizeof(g->title_id)) != 0);
            CHECK(std::memchr(g->version, 0, sizeof(g->version)) != 0);
            CHECK(std::memchr(g->region, 0, sizeof(g->region)) != 0);
        }
    }

    /* 3. ParseName fuzz: long, bracket-heavy, unterminated-looking names. */
    {
        static char name[4096];
        for (int iter = 0; iter < 200000; ++iter) {
            uint32_t len = Rnd() % 4000;
            FillRandom((unsigned char *)name, len);
            /* Force printable-ish content plus brackets to drive the tag loop. */
            if (len > 0 && (Rnd() & 1)) {
                static const char alpha[] =
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789[] ._-+()";
                for (uint32_t i = 0; i < len; ++i)
                    name[i] = alpha[Rnd() % (sizeof(alpha) - 1)];
            }
            name[len] = 0;
            Game g;
            std::memset(&g, 0, sizeof(g));
            Library_ParseName(name, &g);
            /* If it kept any data, the fields must stay NUL-terminated. */
            if (g.title[0])  CHECK(std::memchr(g.title, 0, sizeof(g.title)) != 0);
            if (g.version[0])CHECK(std::memchr(g.version, 0, sizeof(g.version)) != 0);
            if (g.region[0]) CHECK(std::memchr(g.region, 0, sizeof(g.region)) != 0);
            if (g.title_id[0])CHECK(std::memchr(g.title_id, 0, sizeof(g.title_id)) != 0);
            (void)Library_IsGameExt(name);
        }
        std::printf("ParseName fuzz done\n");
    }

    /* 4. ScanContainer fuzz: random bytes, plus PFS0/NSO/NRO headers. */
    {
        static unsigned char buf[4096];
        for (int iter = 0; iter < 200000; ++iter) {
            uint32_t len = Rnd() % sizeof(buf);
            FillRandom(buf, len);
            switch (Rnd() % 5) {
            case 0: if (len >= 4) { buf[0]='P';buf[1]='F';buf[2]='S';buf[3]='0'; } break;
            case 1: if (len >= 4) { buf[0]='N';buf[1]='S';buf[2]='O';buf[3]='0'; } break;
            case 2: if (len >= 4) { buf[0]='N';buf[1]='R';buf[2]='O';buf[3]='0'; } break;
            case 3: if (len >= 0x104) { std::memset(buf + 0x100, 0, 4); buf[0x100]='H';buf[0x101]='E';buf[0x102]='A';buf[0x103]='D'; } break;
            default: break;
            }
            Game g;
            std::memset(&g, 0, sizeof(g));
            Library_ScanContainer(buf, len, &g);
            ModuleInfo m;
            Loader_ProbeModule(buf, len, &m);
        }
        std::printf("ScanContainer/Probe fuzz done\n");
    }

    if (g_fail) { std::printf("%d test(s) FAILED\n", g_fail); return 1; }
    std::printf("All scan-path fuzz checks passed\n");
    return 0;
}
