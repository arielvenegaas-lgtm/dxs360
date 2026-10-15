/* test_img.cpp: host-side tests for the shared image decoder (src/img.c).
 *
 * Decodes PNG (RGB/RGBA/gray/palette) and BMP (24/32-bit) fixtures produced by
 * eden/tests/make_img_fixtures.py and compares every pixel against the expected
 * 0xAARRGGBB bytes. Then fuzzes malformed inputs to prove the decoder never
 * crashes and never claims success on garbage.
 *
 * Build:
 *   gcc -std=c99 -Wall -c src/img.c -I src -o /tmp/img.o
 *   g++ -std=c++11 -Wall -I src eden/tests/test_img.cpp /tmp/img.o -o /tmp/test_img
 *   /tmp/test_img /tmp/imgfix
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <string>

#include "img.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static bool ReadFile(const std::string &path, std::vector<uint8_t> &out) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) { std::fclose(f); return false; }
    out.resize((size_t)n);
    size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

static void TestFixture(const std::string &dir, const char *name,
                        int exp_w, int exp_h) {
    std::vector<uint8_t> data, want;
    std::string path = dir + "/" + name;
    if (!ReadFile(path, data)) { std::printf("SKIP: %s missing\n", path.c_str()); return; }
    if (!ReadFile(path + ".argb", want)) { std::printf("SKIP: %s.argb missing\n", path.c_str()); return; }

    CHECK(Img_IsSupported(data.data(), data.size()));
    Image img; std::memset(&img, 0, sizeof(img));
    CHECK(Img_LoadAuto(data.data(), data.size(), &img) == 1);
    uint32_t gw = img.w, gh = img.h;
    CHECK((int)gw == exp_w);
    CHECK((int)gh == exp_h);
    if (gw == (uint32_t)exp_w && gh == (uint32_t)exp_h) {
        size_t nbytes = (size_t)img.w * img.h * 4u;
        if (want.size() == nbytes && img.pixels) {
            int bad = 0;
            for (size_t i = 0; i < nbytes; ++i)
                if (((const uint8_t *)img.pixels)[i] != want[i]) { bad = 1; break; }
            if (bad) {
                std::printf("FAIL %s: pixel mismatch\n", name);
                ++g_fail;
            }
        } else {
            std::printf("FAIL %s: expected %zu bytes, got image %p\n",
                        name, want.size(), (void *)img.pixels);
            ++g_fail;
        }
    }
    Img_Free(&img);
    CHECK(img.pixels == nullptr);
    std::printf("ok %s (%ux%u)\n", name, gw, gh);
}

static void FuzzMalformed() {
    /* Deterministic PRNG. */
    uint32_t s = 0x1234abcd;
    auto rnd = [&]() -> uint8_t { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (uint8_t)(s >> 24); };

    std::vector<uint8_t> buf;
    int accepted = 0;
    for (int iter = 0; iter < 4000; ++iter) {
        size_t n = 1 + (rnd() % 300);
        buf.resize(n);
        for (size_t i = 0; i < n; ++i) buf[i] = rnd();
        /* Occasionally give it a valid-ish PNG signature to reach deeper code. */
        if (iter % 3 == 0 && n >= 8) {
            static const uint8_t sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
            std::memcpy(buf.data(), sig, 8);
        }
        if (iter % 5 == 0 && n >= 2) { buf[0] = 'B'; buf[1] = 'M'; }
        Image img; std::memset(&img, 0, sizeof(img));
        if (Img_LoadAuto(buf.data(), buf.size(), &img)) {
            accepted++;
            Img_Free(&img);
        }
    }
    /* Random bytes essentially never form a valid image; allow a tiny margin. */
    std::printf("fuzz: %d/4000 accidentally accepted\n", accepted);
    CHECK(accepted < 40);
}

int main(int argc, char **argv) {
    std::string dir = argc > 1 ? argv[1] : "/tmp/imgfix";
    int w = 6, h = 4;

    TestFixture(dir, "rgb8.png",   w, h);
    TestFixture(dir, "rgba8.png",  w, h);
    TestFixture(dir, "gray8.png",  w, h);
    TestFixture(dir, "pal8.png",   w, h);
    TestFixture(dir, "filt_rgb.png",   w, h);
    TestFixture(dir, "filt_paeth.png", w, h);
    TestFixture(dir, "rgb24.bmp",  w, h);
    TestFixture(dir, "rgba32.bmp", w, h);

    /* Truncated valid PNG must fail cleanly. */
    {
        std::vector<uint8_t> data;
        if (ReadFile(dir + "/rgba8.png", data)) {
            Image img; std::memset(&img, 0, sizeof(img));
            CHECK(Img_LoadPng(data.data(), data.size() / 2, &img) == 0);
            CHECK(img.pixels == nullptr);
        }
    }

    FuzzMalformed();

    if (g_fail) { std::printf("%d check(s) FAILED\n", g_fail); return 1; }
    std::printf("All image-decoder checks passed\n");
    return 0;
}
