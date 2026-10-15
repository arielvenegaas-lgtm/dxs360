/* test_artwork.cpp: host-side tests for cover-art lookup (eden/source/game).
 *
 * Verifies candidate generation, that a real PNG on disk is found and decoded,
 * that a missing/broken image degrades to "no art" (never fake), and that the
 * result is downscaled to the requested bounds.
 *
 * Build:
 *   gcc -std=c99 -Wall -c src/img.c -I src -o /tmp/img.o
 *   g++ -std=c++11 -Wall -I src -I eden/source/game \
 *       eden/tests/test_artwork.cpp eden/source/game/artwork.cpp /tmp/img.o -o /tmp/test_art
 *   /tmp/test_art /tmp/imgfix
 */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <map>
#include <vector>

#include "artwork.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

/* --- fake filesystem: path -> bytes ------------------------------------- */
static std::map<std::string, std::vector<uint8_t>> g_fs;
struct FakeCtx { int reads; };
static long FakeRead(void *user, const char *path, void *buf, uint32_t max) {
    FakeCtx *c = (FakeCtx *)user;
    if (c) c->reads++;
    auto it = g_fs.find(path);
    if (it == g_fs.end()) return -1;
    size_t n = it->second.size();
    if (n > max) n = max;
    std::memcpy(buf, it->second.data(), n);
    return (long)n;
}

static bool LoadDisk(const std::string &p, std::vector<uint8_t> &out) {
    FILE *f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    out.resize((size_t)n);
    size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

int main(int argc, char **argv) {
    std::string fixdir = argc > 1 ? argv[1] : "/tmp/imgfix";

    /* Candidate generation. */
    char cand[ARTWORK_MAX_CAND][256];
    int n = Artwork_Candidates("roms", "Cave Story+ [0100B7D0022EE000][v0][US].nsp",
                               cand, ARTWORK_MAX_CAND, 256);
    bool has_cover = false, has_named = false, has_folder = false;
    for (int i = 0; i < n; ++i) {
        if (!std::strcmp(cand[i], "roms/cover.png")) has_cover = true;
        if (std::strstr(cand[i], "Cave Story+ [0100B7D0022EE000][v0][US].png")) has_named = true;
        if (std::strstr(cand[i], "Cave Story+ [0100B7D0022EE000][v0][US]/cover.png")) has_folder = true;
    }
    CHECK(n > 0);
    CHECK(has_cover);
    CHECK(has_named);
    CHECK(has_folder);

    /* No dir: candidates are just the bare names. */
    n = Artwork_Candidates("", "a.nsp", cand, ARTWORK_MAX_CAND, 256);
    CHECK(n > 0);
    CHECK(!std::strcmp(cand[0], "cover.png"));

    /* Real PNG found via the "game-name.png" candidate. */
    std::vector<uint8_t> png;
    if (LoadDisk(fixdir + "/rgb8.png", png)) {
        g_fs.clear();
        g_fs["roms/cover.png"] = {};                    /* empty -> read 0 */
        g_fs["roms/MyGame.nsp.png"] = png;              /* the real one */
        FakeCtx ctx{0};
        Image img; std::memset(&img, 0, sizeof(img));
        char used[256] = {0};
        int ok = Artwork_LoadForGame(FakeRead, &ctx, "roms", "MyGame.nsp",
                                     128, 128, &img, used, sizeof(used));
        CHECK(ok == 1);
        CHECK(img.w == 6 && img.h == 4);                /* smaller than bound */
        CHECK(!std::strcmp(used, "roms/MyGame.nsp.png"));
        Img_Free(&img);
    } else {
        std::printf("SKIP: %s/rgb8.png missing\n", fixdir.c_str());
    }

    /* Missing art -> honest failure. */
    {
        g_fs.clear();
        FakeCtx ctx{0};
        Image img; std::memset(&img, 0, sizeof(img));
        CHECK(Artwork_LoadForGame(FakeRead, &ctx, "roms", "None.nsp",
                                  128, 128, &img, nullptr, 0) == 0);
    }

    /* Present but not an image -> honest failure (no fake art). */
    {
        g_fs.clear();
        g_fs["roms/cover.png"] = {'n', 'o', 'p', 'e'};
        FakeCtx ctx{0};
        Image img; std::memset(&img, 0, sizeof(img));
        CHECK(Artwork_LoadForGame(FakeRead, &ctx, "roms", "Broken.nsp",
                                  128, 128, &img, nullptr, 0) == 0);
    }

    /* A large image is downscaled to fit the bound. */
    if (LoadDisk(fixdir + "/rgba8.png", png)) {
        g_fs.clear();
        g_fs["roms/Big.nsp/cover.png"] = png;
        FakeCtx ctx{0};
        Image img; std::memset(&img, 0, sizeof(img));
        int ok = Artwork_LoadForGame(FakeRead, &ctx, "roms", "Big.nsp",
                                     3, 2, &img, nullptr, 0);
        CHECK(ok == 1);
        CHECK(img.w <= 3 && img.h <= 2 && img.w > 0 && img.h > 0);
        Img_Free(&img);
    }

    if (g_fail) { std::printf("%d check(s) FAILED\n", g_fail); return 1; }
    std::printf("All artwork checks passed\n");
    return 0;
}
