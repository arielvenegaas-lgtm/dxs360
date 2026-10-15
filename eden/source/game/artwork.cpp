/* artwork.cpp: see artwork.h. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "artwork.h"

/* Strips the directory and the last extension from `name`, into `base`. */
static void BaseName(const char *name, char *base, int base_sz) {
    const char *slash = strrchr(name, '/');
    const char *b = slash ? slash + 1 : name;
    int n = (int)strlen(b);
    /* drop a final ".ext" (but keep a leading dot-only name untouched) */
    const char *dot = strrchr(b, '.');
    if (dot && dot != b) n = (int)(dot - b);
    if (n > base_sz - 1) n = base_sz - 1;
    memcpy(base, b, (size_t)n);
    base[n] = 0;
}

int Artwork_Candidates(const char *dir, const char *file,
                       char out[][256], int max, int pathsz) {
    if (max <= 0) return 0;
    if (pathsz > 256) pathsz = 256;
    if (!file) file = "";

    char base[128];
    BaseName(file, base, (int)sizeof(base));

    /* Directory prefix with a single trailing slash (or empty). */
    char prefix[160];
    int pn = 0;
    if (dir && dir[0]) {
        for (; dir[pn] && pn < (int)sizeof(prefix) - 2; ++pn) prefix[pn] = dir[pn];
        if (pn == 0 || prefix[pn - 1] != '/') prefix[pn++] = '/';
    }
    prefix[pn] = 0;

    const char *names[ARTWORK_MAX_CAND];
    char base_files[ARTWORK_MAX_CAND][160];
    int n = 0;

    /* Just the file's basename (extension kept), for covers named after it. */
    char full[128];
    const char *fslash = strrchr(file, '/');
    const char *fb = fslash ? fslash + 1 : file;
    int fn = (int)strlen(fb);
    if (fn > (int)sizeof(full) - 1) fn = (int)sizeof(full) - 1;
    memcpy(full, fb, (size_t)fn);
    full[fn] = 0;

    /* Flat covers in the game's directory. */
    names[n++] = "cover.png";
    names[n++] = "cover.jpg";
    names[n++] = "cover.jpeg";
    names[n++] = "folder.png";
    /* A file named after the game. */
    if (base[0]) {
        snprintf(base_files[0], sizeof(base_files[0]), "%s.png", base);
        snprintf(base_files[1], sizeof(base_files[1]), "%s.jpg", base);
        snprintf(base_files[2], sizeof(base_files[2]), "%s.jpeg", base);
        names[n++] = base_files[0];
        names[n++] = base_files[1];
        names[n++] = base_files[2];
        /* A folder named after the game with a cover inside. */
        snprintf(base_files[3], sizeof(base_files[3]), "%s/cover.png", base);
        snprintf(base_files[4], sizeof(base_files[4]), "%s/cover.jpg", base);
        names[n++] = base_files[3];
        names[n++] = base_files[4];
    }
    /* A file named after the full filename (e.g. "Game.nsp.png"). */
    if (full[0]) {
        snprintf(base_files[5], sizeof(base_files[5]), "%s.png", full);
        snprintf(base_files[6], sizeof(base_files[6]), "%s.jpg", full);
        snprintf(base_files[7], sizeof(base_files[7]), "%s/cover.png", full);
        names[n++] = base_files[5];
        names[n++] = base_files[6];
        names[n++] = base_files[7];
    }

    int count = 0;
    for (int i = 0; i < n && count < max; ++i) {
        snprintf(out[count], (size_t)pathsz, "%s%s", prefix, names[i]);
        count++;
    }
    return count;
}

int Artwork_LoadForGame(ArtReadFn read, void *user,
                        const char *dir, const char *file,
                        uint32_t maxW, uint32_t maxH,
                        Image *out, char *used, int usedsz) {
    if (!read || !out) return 0;

    char cand[ARTWORK_MAX_CAND][256];
    int n = Artwork_Candidates(dir, file, cand, ARTWORK_MAX_CAND, 256);

    /* Cover files are small; cap the read to keep the console heap sane. */
    const uint32_t kMaxBytes = 6u * 1024u * 1024u;
    unsigned char *buf = (unsigned char *)malloc(kMaxBytes);
    if (!buf) return 0;

    int ok = 0;
    for (int i = 0; i < n && !ok; ++i) {
        long got = read(user, cand[i], buf, kMaxBytes);
        if (got <= 0) continue;
        Image full;
        if (!Img_LoadAuto(buf, (size_t)got, &full)) continue; /* not an image */
        if (Img_ScaleToFit(&full, maxW, maxH, out)) {
            if (used && usedsz > 0) {
                int k = 0;
                for (; cand[i][k] && k < usedsz - 1; ++k) used[k] = cand[i][k];
                used[k] = 0;
            }
            ok = 1;
        }
        Img_Free(&full);
    }
    free(buf);
    return ok;
}
