/* artwork.h: local cover-art lookup for the Eden360 frontend.
 *
 * Games may ship a cover image next to them; Eden looks for common names and
 * decodes the first one it can (PNG/BMP via src/img.c). Nothing is fetched from
 * the network and no artwork is invented: if nothing is found or the file is
 * not a supported image, the caller falls back to its placeholder cover.
 *
 * The file reader is injected so this module stays free of hardware deps and
 * can be unit-tested on the host.
 */
#pragma once

#include <stdint.h>

#include "img.h"

typedef struct ArtworkCtx ArtworkCtx;

/* Reads a whole file at `path` into `buf` (up to `max` bytes). Returns bytes
 * read, or -1 if not found / unreadable. */
typedef long (*ArtReadFn)(void *user, const char *path, void *buf, uint32_t max);

/* Builds the candidate cover paths for `dir`/`file` into `out` (an array of
 * `max` C strings, each up to pathsz bytes). Returns the count. `file` may be
 * a basename with or without directory. Pure: no I/O. */
int Artwork_Candidates(const char *dir, const char *file,
                       char out[][256], int max, int pathsz);

/* Loads the first decodable cover for `dir`/`file`. On success fills `out`
 * (caller frees with Img_Free) and copies the chosen path into `used`
 * (may be NULL). Images are downscaled to fit maxW x maxH. Returns 1 on
 * success, 0 if no usable artwork was found. */
int Artwork_LoadForGame(ArtReadFn read, void *user,
                        const char *dir, const char *file,
                        uint32_t maxW, uint32_t maxH,
                        Image *out, char *used, int usedsz);

/* The maximum number of candidate paths Artwork_Candidates will produce. */
#define ARTWORK_MAX_CAND 16
