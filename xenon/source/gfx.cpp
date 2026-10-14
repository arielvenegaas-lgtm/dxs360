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

/* Per-column tiled offset: the parts of the micro-tile index that depend only
 * on x. Computed once and reused every row/blit. */
static uint32_t s_xoff[GFX_MAX_W];
static int      s_xoff_ready = 0;

/* Blit the linear shadow into the micro-tiled scan-out buffer. */
static void Gfx_Blit(GfxContext *g) {
    uint32_t *dst = g->cfb;
    uint32_t  cw  = g->cw;
    uint32_t  w   = g->w;
    uint32_t  h   = g->h;

    if (!s_xoff_ready) {
        for (uint32_t x = 0; x < GFX_MAX_W; ++x)
            s_xoff[x] = ((x >> 5) << 10) + (x & 3u) + (((x & 31u) >> 2) << 3);
        s_xoff_ready = 1;
    }

    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t *row = g->shadow + (size_t)y * w;
        uint32_t rowbase = (y >> 5) * 32u * cw
                         + ((y & 1u) << 2)          /* bit 2      */
                         + (((y & 31u) >> 1) << 6); /* bits 6..   */
        uint32_t yxor = (y & 8u) << 2;              /* bit 5 flip */

        for (uint32_t x = 0; x < w; ++x) {
            uint32_t c = row[x];
            uint32_t r  = (c >> 16) & 0xFFu;
            uint32_t gg = (c >> 8) & 0xFFu;
            uint32_t b  = c & 0xFFu;
            /* Same channel packing as libxenon's console_pset32(). */
            uint32_t val = (b << 24) | (gg << 16) | (r << 8);

            dst[(rowbase + s_xoff[x]) ^ yxor] = val;
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
    int stride = (int)g->w;
    for (int yy = y; yy < y + h; ++yy) {
        if (yy < 0 || (uint32_t)yy >= g->h) continue;
        uint32_t *row = g->shadow + (size_t)yy * stride;
        for (int xx = x; xx < x + w; ++xx) {
            if (xx < 0 || (uint32_t)xx >= g->w) continue;
            row[xx] = argb;
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
