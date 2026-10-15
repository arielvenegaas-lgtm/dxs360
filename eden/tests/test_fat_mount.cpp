/* test_fat_mount.cpp: host-side tests for FAT32 volume/partition selection.
 *
 * Verifies the read-only FAT32 parser picks the right volume on tricky drives:
 *   - a superfloppy (no partition table),
 *   - a single MBR partition,
 *   - a multi-partition MBR where the FIRST FAT32 partition is empty and the
 *     game data lives on a later partition (the common USB layout that used to
 *     make Eden report "no roms" / "prod.keys not found").
 *
 * The image is supplied as a file argument so the CI can build real images with
 * mkfs.fat/mtools. Build:
 *   gcc -std=c99 -Wall -c xenon/source/fat32.c -I xenon/source -o /tmp/fat32.o
 *   g++ -std=c++11 -Wall -I xenon/source \
 *       eden/tests/test_fat_mount.cpp /tmp/fat32.o -o /tmp/test_fat_mount
 *   /tmp/test_fat_mount /tmp/multi.img roms "Cave Story+ [0100B7D0022EE000][v0][US].nsp"
 */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>

#include "fat32.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static FILE *g_img;
static int ReadSec(void *ctx, uint32_t lba, uint32_t cnt, void *buf) {
    (void)ctx;
    if (std::fseek(g_img, (long)lba * 512, SEEK_SET)) return 0;
    return std::fread(buf, 512, cnt, g_img) == cnt;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/tmp/multi.img";
    const char *sub  = argc > 2 ? argv[2] : "roms";
    const char *file = argc > 3 ? argv[3] : nullptr;

    g_img = std::fopen(path, "rb");
    if (!g_img) { std::printf("SKIP: cannot open %s\n", path); return 0; }

    CHECK(Fat32_Mount(ReadSec, nullptr) == 1);

    long n = Fat32_ListDir(sub, nullptr, nullptr);
    std::printf("ListDir(%s) = %ld\n", sub, n);
    CHECK(n >= 0);              /* the data partition must be the one mounted */

    if (file) {
        char full[512];
        std::snprintf(full, sizeof(full), "%s/%s", sub, file);
        char buf[64];
        long got = Fat32_ReadFile(full, buf, sizeof(buf));
        std::printf("ReadFile(%s) = %ld\n", full, got);
        CHECK(got >= 0);
    }

    if (g_fail) { std::printf("%d check(s) FAILED\n", g_fail); return 1; }
    std::printf("OK\n");
    return 0;
}
