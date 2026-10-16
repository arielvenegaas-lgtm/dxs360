/* test_fat_write.cpp: host test for the FAT32 writer (xenon/source/fat32.c).
 *
 * Backs a real image file with read+write sector callbacks, then:
 *   1. mkfs.fat a FAT32 image,
 *   2. mkdir / write short + long (LFN) names, nested dirs and a larger
 *      streamed file,
 *   3. reads everything back with our own reader and compares bytes,
 *   4. re-mounts to prove persistence,
 *   5. verifies the result with the system tools (fsck.fat + mtools) so we
 *      know our on-disk format is real FAT32, not just readable by us,
 *   6. checks free-space accounting and that an over-size write fails cleanly
 *      (no partial file, filesystem still consistent).
 *
 * Build:  g++ -std=c++11 -Wall -I xenon/source test_fat_write.cpp fat32.c -o ...
 * Run:    test_fat_write [tmpdir]
 * Requires: mkfs.fat, fsck.fat, mdir, mtype on PATH (dosfstools + mtools).
 */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" {
#include "fat32.h"
}

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

struct FileImg {
    FILE *f;
    int Rd(void *ctx, uint32_t lba, uint32_t n, void *buf) {
        (void)ctx;
        std::fseek(this->f, (long)lba * 512, SEEK_SET);
        return std::fread(buf, 512, n, this->f) == n;
    }
    int Wr(void *ctx, uint32_t lba, uint32_t n, const void *buf) {
        (void)ctx;
        std::fseek(this->f, (long)lba * 512, SEEK_SET);
        return std::fwrite(buf, 512, n, this->f) == n;
    }
};
static int RdTramp(void *ctx, uint32_t lba, uint32_t n, void *buf) {
    return ((FileImg *)ctx)->Rd(ctx, lba, n, buf);
}
static int WrTramp(void *ctx, uint32_t lba, uint32_t n, const void *buf) {
    return ((FileImg *)ctx)->Wr(ctx, lba, n, buf);
}

static bool Run(const std::string &cmd, bool quiet = true) {
    std::string c = cmd;
    if (quiet) c += " >/dev/null 2>&1";
    return std::system(c.c_str()) == 0;
}

static bool HaveTool(const char *t) {
    return Run(std::string("command -v ") + t);
}

int main(int argc, char **argv) {
    std::string dir = argc > 1 ? argv[1] : "/tmp";
    std::string img = dir + "/fatw.img";

    if (!HaveTool("mkfs.fat")) { std::printf("SKIP: mkfs.fat not found\n"); return 0; }

    /* Fresh 64 MB FAT32 image (1 KiB clusters => >65525 clusters). */
    std::remove(img.c_str());
    if (!Run("mkfs.fat -C -F 32 -s 2 " + img + " 65536")) {
        std::printf("SKIP: mkfs.fat failed\n"); return 0;
    }

    FileImg fi;
    fi.f = std::fopen(img.c_str(), "r+b");
    CHECK(fi.f != nullptr);
    if (!fi.f) return 1;

    /* Probe should identify FAT32. */
    CHECK(Fat32_Probe(RdTramp, &fi) == FAT32_FS_FAT32);

    Fat32 *fs = Fat32_Open(RdTramp, WrTramp, &fi);
    CHECK(fs != nullptr);
    CHECK(Fat32_MountVolume(fs) == 1);
    CHECK(Fat32_IsWritable(fs) == 1);

    uint64_t total = Fat32_TotalBytes(fs);
    uint64_t free0 = Fat32_FreeBytes(fs);
    std::printf("mounted: total=%lluMB free=%lluMB\n",
                (unsigned long long)(total / (1024 * 1024)),
                (unsigned long long)(free0 / (1024 * 1024)));
    CHECK(total > 32u * 1024 * 1024);
    CHECK(free0 > total / 2);

    /* mkdir */
    CHECK(Fat32V_Mkdir(fs, "Switch") == 1);
    CHECK(Fat32V_Mkdir(fs, "Switch/Games") == 1);
    CHECK(Fat32V_Exists(fs, "Switch"));
    int isd = 0; CHECK(Fat32V_Stat(fs, "Switch", nullptr, &isd) && isd);
    /* mkdir is idempotent for an existing dir */
    CHECK(Fat32V_Mkdir(fs, "Switch") == 1);

    /* small file */
    const char *hello = "hello fat32 writer\n";
    CHECK(Fat32V_WriteFile(fs, "Switch/hello.txt",
                           hello, (uint32_t)std::strlen(hello)) == 1);

    /* long filename needing LFN entries */
    const char *longname = "Switch/Games/Cave Story+ [0100B7D0022EE000][v0][US].nsp";
    std::vector<uint8_t> big(200000);
    for (size_t i = 0; i < big.size(); ++i) big[i] = (uint8_t)(i * 31 + 7);
    CHECK(Fat32V_WriteFile(fs, longname, big.data(), (uint32_t)big.size()) == 1);

    /* streaming writer, chunked */
    {
        Fat32File *w = Fat32File_Create(fs, "Switch/stream.bin");
        CHECK(w != nullptr);
        size_t off = 0;
        int ok = 1;
        while (off < big.size()) {
            uint32_t chunk = (uint32_t)((big.size() - off) < 1000 ? (big.size() - off) : 1000);
            if (!Fat32File_Write(w, big.data() + off, chunk)) { ok = 0; break; }
            off += chunk;
        }
        CHECK(ok);
        CHECK(Fat32File_Finish(w) == 1);
    }

    /* read back + compare */
    {
        char buf[64];
        long n = Fat32V_ReadFile(fs, "Switch/hello.txt", buf, sizeof(buf));
        CHECK(n == (long)std::strlen(hello));
        CHECK(std::memcmp(buf, hello, (size_t)n) == 0);
    }
    {
        std::vector<uint8_t> rd(big.size());
        long n = Fat32V_ReadFile(fs, longname, rd.data(), (uint32_t)rd.size());
        CHECK(n == (long)big.size());
        CHECK(std::memcmp(rd.data(), big.data(), big.size()) == 0);
    }
    {
        std::vector<uint8_t> rd(big.size());
        long n = Fat32V_ReadFile(fs, "Switch/stream.bin", rd.data(), (uint32_t)rd.size());
        CHECK(n == (long)big.size());
        CHECK(std::memcmp(rd.data(), big.data(), big.size()) == 0);
    }

    /* free space should have dropped */
    uint64_t free1 = Fat32_FreeBytes(fs);
    CHECK(free1 < free0);

    /* streaming reader: read the big file in odd-sized chunks and compare */
    {
        uint32_t sz = 0;
        Fat32Reader *r = Fat32Reader_Open(fs, longname, &sz);
        CHECK(r != nullptr);
        CHECK(sz == (uint32_t)big.size());
        std::vector<uint8_t> rd;
        uint8_t chunk[777];
        for (;;) {
            long got = Fat32Reader_Read(r, chunk, sizeof(chunk));
            CHECK(got >= 0);
            if (got <= 0) break;
            rd.insert(rd.end(), chunk, chunk + got);
        }
        Fat32Reader_Close(r);
        CHECK(rd.size() == big.size());
        CHECK(std::memcmp(rd.data(), big.data(), big.size()) == 0);
    }

    /* listing finds our entries */
    struct Cnt { int files, dirs; int saw_hello; };
    Cnt c{0, 0, 0};
    Fat32V_ListDir(fs, "Switch", [](void *ctx, const char *name, uint32_t, int is_dir) -> int {
        Cnt *c = (Cnt *)ctx;
        if (is_dir) c->dirs++; else c->files++;
        if (!std::strcmp(name, "hello.txt")) c->saw_hello = 1;
        return 1;
    }, &c);
    CHECK(c.saw_hello);

    Fat32_Close(fs);
    std::fclose(fi.f);

    /* Re-mount: persistence. */
    fi.f = std::fopen(img.c_str(), "r+b");
    fs = Fat32_Open(RdTramp, WrTramp, &fi);
    CHECK(Fat32_MountVolume(fs) == 1);
    {
        std::vector<uint8_t> rd(big.size());
        long n = Fat32V_ReadFile(fs, longname, rd.data(), (uint32_t)rd.size());
        CHECK(n == (long)big.size());
        CHECK(std::memcmp(rd.data(), big.data(), big.size()) == 0);
    }
    Fat32_Close(fs);
    std::fclose(fi.f);

    /* fsck.fat and mtools confirm real FAT32 on disk. */
    if (HaveTool("fsck.fat")) {
        CHECK(Run("fsck.fat -n " + img));
    }
    if (HaveTool("mdir")) {
        CHECK(Run("MTOOLS_SKIP_CHECK=1 mdir -i " + img + " ::/Switch"));
        CHECK(Run("MTOOLS_SKIP_CHECK=1 mdir -i " + img + " ::/Switch/Games"));
    }

    /* --- over-size write must fail cleanly on a tiny image --- */
    {
        std::string small = dir + "/fatw_small.img";
        std::remove(small.c_str());
        Run("mkfs.fat -C -F 32 -s 1 " + small + " 4096"); /* 4 MB */
        FileImg si;
        si.f = std::fopen(small.c_str(), "r+b");
        Fat32 *sf = Fat32_Open(RdTramp, WrTramp, &si);
        CHECK(Fat32_MountVolume(sf) == 1);
        std::vector<uint8_t> huge(8u * 1024 * 1024, 0x5A); /* 8 MB into 4 MB */
        int ok = Fat32V_WriteFile(sf, "too_big.bin", huge.data(), (uint32_t)huge.size());
        CHECK(ok == 0);
        /* no partial file left behind */
        CHECK(Fat32V_Exists(sf, "too_big.bin") == 0);
        Fat32_Close(sf);
        std::fclose(si.f);
        if (HaveTool("fsck.fat")) CHECK(Run("fsck.fat -n " + small));
        std::remove(small.c_str());
    }

    /* --- FATX detection --- */
    {
        static uint8_t fatx[512];
        std::memset(fatx, 0, sizeof(fatx));
        fatx[0]='F'; fatx[1]='A'; fatx[2]='T'; fatx[3]='X';
        Fat32ReadSectors rd = [](void *ctx, uint32_t lba, uint32_t n, void *buf) -> int {
            (void)lba; (void)n;
            std::memcpy(buf, ctx, 512);
            return 1;
        };
        CHECK(Fat32_Probe(rd, fatx) == FAT32_FS_FATX);
    }

    std::remove(img.c_str());

    if (g_fail) { std::printf("%d check(s) FAILED\n", g_fail); return 1; }
    std::printf("All FAT32 writer checks passed\n");
    return 0;
}
