/* gfx.cpp: libXenon (bare-metal) renderer for Xbox 360.
 *
 * The D3D9 build uploads a CPU shadow framebuffer into a tiled GPU texture and
 * blits it with a shader. libXenon has no D3D9, but its console driver exposes
 * the GPU scan-out framebuffer as a CPU-addressable, micro-tiled surface.
 *
 * Approach:
 *   1. xenos_init() programs the video mode; console_init() reads the scan-out
 *      geometry straight out of the GPU registers (see libxenon console.c).
 *   2. We keep a linear 0xAARRGGBB shadow framebuffer, exactly like the D3D9
 *      build, so game.cpp is untouched.
 *   3. Each frame we blit the shadow into the tiled scan-out buffer using the
 *      same micro-tiling formula as libxenon's console_pset32(), then flush the
 *      data cache so the display engine sees the new pixels.
 */
#include <stdint.h>
#include <string.h>

#include <xenos/xenos.h>
#include <console/console.h>

/* libXenon's cache.h declares memdcbst() with C linkage only (no extern "C"
 * guard), so we must include it inside an extern "C" block when building C++,
 * otherwise the reference is name-mangled and fails to link. */
extern "C" {
#include <ppc/cache.h>
}

#include "gfx.h"
#include "font.h"

/* Largest scan-out we support a shadow buffer for (1080p). */
#define GFX_MAX_W 1920
#define GFX_MAX_H 1088

struct GfxContext {
    uint32_t  w, h;      /* logical (scan-out) size            */
    uint32_t  cw, ch;    /* tiled pitch (w rounded up to 32)   */
    uint32_t *shadow;    /* linear 0xAARRGGBB framebuffer      */
    uint32_t *cfb;       /* tiled scan-out framebuffer         */
    int       pitch;     /* shadow row pitch, in bytes         */
    int       ready;
};

static GfxContext g_ctx;
static uint32_t  g_shadow[GFX_MAX_W * GFX_MAX_H];

/*
 * libxenon's console.c reads the scan-out surface address and size directly
 * from the GPU's display registers (the "struct ati_info" at 0xec806100 is
 * really just an overlay over D1GRPH_* MMIO). We mirror that here, reusing the
 * D1GRPH_* offsets that <xenos/xenos.h> already defines.
 */
#define XENOS_REG(i) (*(volatile uint32_t *)(uintptr_t)(0xec800000u + (i)))

static uint32_t RegRead(uint32_t off) { return XENOS_REG(off); }

GfxContext *Gfx_Create(void) {
    GfxContext *g = &g_ctx;
    memset(g, 0, sizeof(*g));

    xenos_init(VIDEO_MODE_AUTO);
    console_init();  /* computes/reads fb geometry; also installs stdout hook */
    console_close(); /* stop printf from stomping on our drawing */

    uint32_t base = RegRead(D1GRPH_PRIMARY_SURFACE_ADDRESS) | 0x80000000u;
    uint32_t w    = RegRead(D1GRPH_X_END);
    uint32_t h    = RegRead(D1GRPH_Y_END);

    if (w < 32u || w > GFX_MAX_W || h < 32u || h > GFX_MAX_H) {
        /* Sensible fallback if the mode registers look bogus. */
        w = 1280u; h = 720u;
    }

    g->w  = w;
    g->h  = h;
    g->cw = (w + 31u) & ~31u;
    g->ch = (h + 31u) & ~31u;
    g->cfb = (uint32_t *)(uintptr_t)base;
    g->shadow = g_shadow;
    g->pitch = (int)(w * 4u);

    memset(g->shadow, 0, (size_t)w * h * 4u);

    g->ready = 1;
    return g;
}

void Gfx_Destroy(GfxContext *g) { (void)g; }

int      Gfx_Ready(GfxContext *g)  { return g && g->ready; }
uint32_t Gfx_Width(GfxContext *g)  { return g ? g->w : 0; }
uint32_t Gfx_Height(GfxContext *g) { return g ? g->h : 0; }

uint32_t *Gfx_Lock(GfxContext *g, int *pitch_bytes) {
    if (!g || !g->ready) return 0;
    if (pitch_bytes) *pitch_bytes = g->pitch;
    return g->shadow;
}

/* Blit the linear shadow into the micro-tiled scan-out buffer.
 *
 * The scan-out buffer is a permutation of linear memory (a full-screen fill is
 * order-independent), so we iterate one 32x32 tile at a time: the destination
 * writes then stay inside a single 4 KB tile instead of striding across ~40
 * tiles per row. That is far friendlier to the store buffer / L2. */
static void Gfx_Blit(GfxContext *g) {
    uint32_t *dst = g->cfb;
    uint32_t  cw  = g->cw;
    uint32_t  w   = g->w;
    uint32_t  h   = g->h;

    for (uint32_t ty = 0; ty < h; ty += 32) {
        uint32_t tylen = (h - ty < 32u) ? (h - ty) : 32u;
        for (uint32_t tx = 0; tx < w; tx += 32) {
            uint32_t txlen = (w - tx < 32u) ? (w - tx) : 32u;

            uint32_t *dtile = dst + (size_t)(ty >> 5) * 32u * cw
                                  + ((size_t)(tx >> 5) << 10);

            for (uint32_t ly = 0; ly < tylen; ++ly) {
                const uint32_t *row = g->shadow + (size_t)(ty + ly) * w + tx;
                uint32_t ypart = ((ly & 1u) << 2) + ((ly >> 1) << 6);
                uint32_t yxor  = (ly & 8u) << 2;

                for (uint32_t lx = 0; lx < txlen; ++lx) {
                    uint32_t c  = row[lx];
                    uint32_t r  = (c >> 16) & 0xFFu;
                    uint32_t gg = (c >> 8) & 0xFFu;
                    uint32_t b  = c & 0xFFu;
                    /* Same channel packing as libxenon's console_pset32(). */
                    uint32_t val = (b << 24) | (gg << 16) | (r << 8);

                    uint32_t off = (lx & 3u) + (((lx >> 2) << 3)) + ypart;
                    dtile[off ^ yxor] = val;
                }
            }
        }
    }

    /* Push our writes out of the data cache to memory for the display engine. */
    memdcbst((void *)dst, (int)((size_t)cw * g->ch * 4u));
}

void Gfx_Present(GfxContext *g) {
    if (!g || !g->ready) return;
    Gfx_Blit(g);
}

/* --- drawing helpers (operate on the linear shadow) --- */

void Gfx_Clear(GfxContext *g, uint32_t argb) {
    if (!g || !g->shadow) return;
    uint32_t *px = g->shadow;
    size_t count = (size_t)g->w * g->h;
    for (size_t i = 0; i < count; ++i) px[i] = argb;
}

void Gfx_FillRect(GfxContext *g, int x, int y, int w, int h, uint32_t argb) {
    if (!g || !g->shadow || w <= 0 || h <= 0) return;

    /* Clip the rectangle to the framebuffer once, up front, so the inner loop
     * is a straight run of stores with no per-pixel bounds test. Gfx_DrawText
     * calls this once per lit font pixel, so the savings add up. */
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > (int)g->w) x1 = (int)g->w;
    int y1 = y + h; if (y1 > (int)g->h) y1 = (int)g->h;
    if (x0 >= x1 || y0 >= y1) return;

    int stride = (int)g->w;
    int span = x1 - x0;
    for (int yy = y0; yy < y1; ++yy) {
        uint32_t *row = g->shadow + (size_t)yy * stride + x0;
        for (int i = 0; i < span; ++i) row[i] = argb;
    }
}

void Gfx_DrawImage(GfxContext *g, int x, int y, int w, int h,
                   const uint32_t *src, int srcw, int srch) {
    if (!g || !g->shadow || !src || w <= 0 || h <= 0 || srcw <= 0 || srch <= 0)
        return;
    int stride = (int)g->w;
    for (int dy = 0; dy < h; ++dy) {
        int yy = y + dy;
        if (yy < 0 || (uint32_t)yy >= g->h) continue;
        int sy = (int)(((long long)dy * srch) / h);
        if (sy >= srch) sy = srch - 1;
        const uint32_t *srow = src + (size_t)sy * srcw;
        uint32_t *drow = g->shadow + (size_t)yy * stride;
        for (int dx = 0; dx < w; ++dx) {
            int xx = x + dx;
            if (xx < 0 || (uint32_t)xx >= g->w) continue;
            int sx = (int)(((long long)dx * srcw) / w);
            if (sx >= srcw) sx = srcw - 1;
            uint32_t s = srow[sx];
            uint32_t a = (s >> 24) & 0xFFu;
            if (a == 0u) continue;
            if (a == 0xFFu) { drow[xx] = 0xFF000000u | (s & 0x00FFFFFFu); continue; }
            uint32_t d = drow[xx];
            uint32_t ia = 255u - a;
            uint32_t r  = (((s >> 16) & 0xFFu) * a + ((d >> 16) & 0xFFu) * ia + 127u) / 255u;
            uint32_t gg = (((s >> 8)  & 0xFFu) * a + ((d >> 8)  & 0xFFu) * ia + 127u) / 255u;
            uint32_t b  = (( s        & 0xFFu) * a + ( d        & 0xFFu) * ia + 127u) / 255u;
            drow[xx] = 0xFF000000u | (r << 16) | (gg << 8) | b;
        }
    }
}

int Gfx_DrawText(GfxContext *g, int x, int y, int scale, uint32_t argb,
                 const char *text) {
    if (!g || !text || scale < 1) return 0;
    int startx = x;
    for (const char *p = text; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n') { x = startx; y += (FONT_GLYPH_H + 1) * scale; continue; }
        const unsigned char *glyph = 0;
        if (c >= FONT_FIRST_CHAR && c <= FONT_LAST_CHAR)
            glyph = FONT5x7[c - FONT_FIRST_CHAR];
        if (glyph) {
            for (int row = 0; row < FONT_GLYPH_H; ++row) {
                unsigned char bits = glyph[row];
                for (int col = 0; col < FONT_GLYPH_W; ++col) {
                    if (bits & (1u << (FONT_GLYPH_W - 1 - col))) {
                        Gfx_FillRect(g, x + col * scale, y + row * scale,
                                     scale, scale, argb);
                    }
                }
            }
        }
        x += (FONT_GLYPH_W + 1) * scale;
    }
    return x - startx;
}
