/* img.c: platform-independent .spr decoder (see img.h). */
#include <stdlib.h>
#include <string.h>

#include "img.h"

static uint32_t RdU32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int Img_LoadSpr(const void *data, size_t len, Image *out) {
    if (!data || !out || len < 20) return 0;
    const uint8_t *b = (const uint8_t *)data;
    if (b[0] != 'H' || b[1] != 'K' || b[2] != 'S' || b[3] != '1') return 0;

    uint32_t ver  = RdU32(b + 4);
    uint32_t w    = RdU32(b + 8);
    uint32_t h    = RdU32(b + 12);
    uint32_t fmt  = RdU32(b + 16);
    if (ver != 1 || fmt != 0 || w == 0 || h == 0) return 0;
    if (w > 4096u || h > 4096u) return 0;
    if (len < 20u + (size_t)w * h * 4u) return 0;

    uint32_t *px = (uint32_t *)malloc((size_t)w * h * 4u);
    if (!px) return 0;

    const uint8_t *s = b + 20;
    size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; ++i) {
        uint32_t r = s[i * 4 + 0];
        uint32_t g = s[i * 4 + 1];
        uint32_t bl = s[i * 4 + 2];
        uint32_t a = s[i * 4 + 3];
        px[i] = (a << 24) | (r << 16) | (g << 8) | bl; /* RGBA -> 0xAARRGGBB */
    }

    out->w = w;
    out->h = h;
    out->pixels = px;
    return 1;
}

void Img_Free(Image *img) {
    if (!img) return;
    free(img->pixels);
    img->pixels = 0;
    img->w = img->h = 0;
}
