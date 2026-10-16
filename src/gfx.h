/* gfx.h: Xbox 360 D3D9 software-framebuffer renderer.
 *
 * A CPU-side 0xAARRGGBB framebuffer is written each frame, tile-uploaded into a
 * GPU texture with XGTileTextureLevel, and blitted fullscreen with a precompiled
 * vertex/pixel shader. This is the same path verified on retail RGH/JTAG
 * hardware in Stu1dd3m0n9/xbox-360-port-enablers (MIT).
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GfxContext GfxContext;

/* Creates the D3D9 device at the console's native video mode. Returns non-NULL
 * even if device creation fails; check Gfx_Ready(). */
GfxContext *Gfx_Create(void);
void        Gfx_Destroy(GfxContext *g);

int         Gfx_Ready(GfxContext *g);
uint32_t    Gfx_Width(GfxContext *g);
uint32_t    Gfx_Height(GfxContext *g);

/* Locks the software framebuffer for the current frame. Returns a pointer to
 * width*height pixels in 0xAARRGGBB (native X8R8G8B8 layout), and optionally
 * the pitch in BYTES. NULL if the device is not ready. */
uint32_t   *Gfx_Lock(GfxContext *g, int *pitch_bytes);

/* Tile-uploads the framebuffer and presents. */
void        Gfx_Present(GfxContext *g);

/* --- drawing helpers (valid between Gfx_Lock and Gfx_Present) --- */

void Gfx_Clear(GfxContext *g, uint32_t argb);
void Gfx_FillRect(GfxContext *g, int x, int y, int w, int h, uint32_t argb);

/* Draws a source image (row-major, straight-alpha 0xAARRGGBB, srcw*srch) into
 * the destination rectangle (x,y,w,h) using nearest-neighbour scaling and
 * per-pixel alpha blending over the framebuffer. */
void Gfx_DrawImage(GfxContext *g, int x, int y, int w, int h,
                   const uint32_t *src, int srcw, int srch);
/* Text is drawn with the built-in 5x7 font. `scale` is the integer pixel size
 * of one font pixel. Returns the advance in pixels. */
int  Gfx_DrawText(GfxContext *g, int x, int y, int scale, uint32_t argb,
                  const char *text);

#ifdef __cplusplus
}
#endif
