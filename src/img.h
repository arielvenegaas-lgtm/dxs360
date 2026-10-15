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
void Img_Free(Image *img);

#ifdef __cplusplus
}
#endif
