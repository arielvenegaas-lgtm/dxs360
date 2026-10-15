/* img.h: minimal image container shared by the D3D9 and libXenon builds.
 *
 * The .spr format is produced by tools/hk_extract.py from a LOCAL game install
 * and carries no engine dependencies:
 *
 *     char  magic[4] = "HKS1"
 *     u32   version  = 1
 *     u32   width
 *     u32   height
 *     u32   format   = 0 (RGBA8888, straight alpha, row-major, top-left origin)
 *     u8    pixels[width*height*4]
 *
 * On load the pixels are converted to the renderer's native 0xAARRGGBB. */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t  w, h;
    uint32_t *pixels; /* w*h, 0xAARRGGBB */
} Image;

/* Parses `data` (len bytes) as a .spr. On success fills `out` and allocates
 * out->pixels (free with Img_Free). Returns 1 on success, 0 on failure. */
int  Img_LoadSpr(const void *data, size_t len, Image *out);

/* Decodes a PNG image (8-bit, non-interlaced; colour types 0/2/3/4/6) into
 * `out`. Handles the full zlib/deflate stream itself. Returns 1 on success. */
int  Img_LoadPng(const void *data, size_t len, Image *out);

/* Decodes an uncompressed BMP (24- or 32-bit BI_RGB, bottom-up or top-down). */
int  Img_LoadBmp(const void *data, size_t len, Image *out);

/* Sniffs the magic and decodes PNG / BMP / .spr accordingly. Returns 1 on
 * success. This is what loaders in the apps should call. */
int  Img_LoadAuto(const void *data, size_t len, Image *out);

/* Returns 1 if `data` looks like an image Img_LoadAuto can decode. */
int  Img_IsSupported(const void *data, size_t len);

/* Returns a new, downscaled copy that fits within maxW x maxH (aspect kept,
 * nearest-neighbour, only shrinks). Returns 0 on failure. Caller frees with
 * Img_Free. */
int  Img_ScaleToFit(const Image *src, uint32_t maxW, uint32_t maxH, Image *out);

void Img_Free(Image *img);

#ifdef __cplusplus
}
#endif
