/* gfx.cpp: D3D9 device + software-framebuffer blit for Xbox 360.
 *
 * Adapted from Stu1dd3m0n9/xbox-360-port-enablers (MIT), where this exact
 * upload/blit path was verified on retail RGH/JTAG hardware. Key hardware
 * constraints encoded here (see the enabler's docs/HW-NOTES.md):
 *   1. D3DXCompileShader hangs the console -> shaders are precompiled (blit_*.h).
 *   2. The backbuffer cannot be LockRect'd -> we own a CPU shadow buffer.
 *   3. Textures are tiled -> upload with XGTileTextureLevel, not memcpy.
 */
#include <xtl.h>
#include <d3dx9.h>
#include <xgraphics.h>
#include <string.h>
#include <stdlib.h>

#include "gfx.h"
#include "font.h"
#include "shaders/blit_vs.h" /* const DWORD g_blit_vs[] */
#include "shaders/blit_ps.h" /* const DWORD g_blit_ps[] */

extern "C" int DbgPrint(const char *fmt, ...);

struct GfxContext {
    Direct3D            *d3d;
    D3DDevice           *dev;
    uint32_t             w, h;        /* content size (native video mode)   */
    uint32_t             tw, th;      /* texture size (h rounded to 32)     */
    uint32_t             tex_w, tex_h;/* driver-reported texture dims       */
    D3DTexture          *tex;
    D3DVertexDeclaration*decl;
    D3DVertexShader     *vsh;
    D3DPixelShader      *psh;
    uint32_t            *shadow;      /* CPU framebuffer                    */
    int                  blit_ok;
    int                  ready;
    int                  pitch;       /* bytes                              */
};

struct BlitVert { float x, y, z, u, v; };

static GfxContext g_ctx;

GfxContext *Gfx_Create(void) {
    GfxContext *g = &g_ctx;
    memset(g, 0, sizeof(*g));

    XVIDEO_MODE vm;
    ZeroMemory(&vm, sizeof(vm));
    XGetVideoMode(&vm);
    g->w = vm.dwDisplayWidth;
    g->h = vm.dwDisplayHeight;
    if (g->w == 0 || g->h == 0) { g->w = 1280; g->h = 720; }
    DbgPrint("[gfx] XGetVideoMode %ux%u wide=%d\n", g->w, g->h,
             (int)vm.fIsWideScreen);

    g->tw = g->w;
    g->th = (g->h + 31u) & ~31u; /* 32-aligned texture height */
    g->pitch = (int)(g->tw * 4);

    g->shadow = (uint32_t *)malloc((size_t)g->tw * g->th * 4);
    if (!g->shadow) { DbgPrint("[gfx] shadow alloc failed\n"); return g; }
    memset(g->shadow, 0, (size_t)g->tw * g->th * 4);

    g->d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g->d3d) { DbgPrint("[gfx] Direct3DCreate9 failed\n"); return g; }

    D3DPRESENT_PARAMETERS pp;
    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth  = g->w;
    pp.BackBufferHeight = g->h;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount  = 1;
    pp.EnableAutoDepthStencil = FALSE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    HRESULT hr = g->d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g->dev);
    if (FAILED(hr) || !g->dev) {
        DbgPrint("[gfx] CreateDevice hr=0x%08x\n", hr);
        g->dev = 0;
        return g;
    }

    D3DVIEWPORT9 vp;
    ZeroMemory(&vp, sizeof(vp));
    vp.Width = g->w; vp.Height = g->h; vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
    g->dev->SetViewport(&vp);

    g->ready = 1;
    DbgPrint("[gfx] device ready %ux%u\n", g->w, g->h);
    return g;
}

void Gfx_Destroy(GfxContext *g) {
    (void)g;
}

int      Gfx_Ready(GfxContext *g)  { return g && g->ready; }
uint32_t Gfx_Width(GfxContext *g)  { return g ? g->w : 0; }
uint32_t Gfx_Height(GfxContext *g) { return g ? g->h : 0; }

static int Gfx_BuildBlit(GfxContext *g) {
    HRESULT hr;
    hr = g->dev->CreateVertexShader((const DWORD *)g_blit_vs, &g->vsh);
    if (FAILED(hr) || !g->vsh) { DbgPrint("[gfx] VS hr=0x%08x\n", hr); return 0; }
    hr = g->dev->CreatePixelShader((const DWORD *)g_blit_ps, &g->psh);
    if (FAILED(hr) || !g->psh) { DbgPrint("[gfx] PS hr=0x%08x\n", hr); return 0; }

    D3DVERTEXELEMENT9 el[] = {
        { 0,  0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };
    hr = g->dev->CreateVertexDeclaration(el, &g->decl);
    if (FAILED(hr) || !g->decl) { DbgPrint("[gfx] decl hr=0x%08x\n", hr); return 0; }

    hr = g->dev->CreateTexture(g->tw, g->th, 1, 0, D3DFMT_X8R8G8B8,
                               D3DPOOL_DEFAULT, &g->tex, NULL);
    if (FAILED(hr) || !g->tex) { DbgPrint("[gfx] tex hr=0x%08x\n", hr); return 0; }

    D3DSURFACE_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    if (SUCCEEDED(g->tex->GetLevelDesc(0, &desc))) {
        g->tex_w = desc.Width;
        g->tex_h = desc.Height;
    }
    g->blit_ok = 1;
    DbgPrint("[gfx] blit ready\n");
    return 1;
}

uint32_t *Gfx_Lock(GfxContext *g, int *pitch_bytes) {
    if (!g || !g->ready) return 0;
    if (!g->blit_ok && !Gfx_BuildBlit(g)) return 0;
    if (pitch_bytes) *pitch_bytes = g->pitch;
    return g->shadow;
}

static void Gfx_Blit(GfxContext *g) {
    D3DLOCKED_RECT lr;
    if (FAILED(g->tex->LockRect(0, &lr, NULL, 0))) {
        DbgPrint("[gfx] tex lock failed\n");
        return;
    }
    /* LockRect exposes raw TILED bytes; tile the linear shadow with XG. */
    XGTileTextureLevel(g->tw, g->th, 0, XGGetGpuFormat(D3DFMT_X8R8G8B8),
                       XGTILE_NONPACKED, lr.pBits, NULL, g->shadow,
                       g->tw * 4, NULL);
    g->tex->UnlockRect(0);

    static BlitVert q[4];
    static int qinit = 0;
    if (!qinit) {
        float hu = 0.5f / (float)g->tw;
        float hv = 0.5f / (float)g->th;
        float v1 = ((float)g->h - 0.5f) / (float)g->th;
        q[0].x = -1; q[0].y =  1; q[0].z = 0; q[0].u = hu;       q[0].v = hv;
        q[1].x =  1; q[1].y =  1; q[1].z = 0; q[1].u = 1.0f - hu; q[1].v = hv;
        q[2].x = -1; q[2].y = -1; q[2].z = 0; q[2].u = hu;       q[2].v = v1;
        q[3].x =  1; q[3].y = -1; q[3].z = 0; q[3].u = 1.0f - hu; q[3].v = v1;
        qinit = 1;
    }

    g->dev->SetVertexDeclaration(g->decl);
    g->dev->SetVertexShader(g->vsh);
    g->dev->SetPixelShader(g->psh);
    g->dev->SetTexture(0, g->tex);
    g->dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g->dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    g->dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    g->dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    g->dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    g->dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(BlitVert));
}

void Gfx_Present(GfxContext *g) {
    if (!g || !g->dev) return;
    if (g->blit_ok && g->shadow) Gfx_Blit(g);
    HRESULT hr = g->dev->Present(NULL, NULL, NULL, NULL);
    if (FAILED(hr)) DbgPrint("[gfx] Present hr=0x%08x\n", hr);
}

/* --- drawing helpers --- */

void Gfx_Clear(GfxContext *g, uint32_t argb) {
    if (!g || !g->shadow) return;
    uint32_t *px = g->shadow;
    size_t count = (size_t)g->tw * g->th;
    for (size_t i = 0; i < count; ++i) px[i] = argb;
}

void Gfx_FillRect(GfxContext *g, int x, int y, int w, int h, uint32_t argb) {
    if (!g || !g->shadow || w <= 0 || h <= 0) return;
    int stride = (int)g->tw;
    for (int yy = y; yy < y + h; ++yy) {
        if (yy < 0 || (uint32_t)yy >= g->th) continue;
        uint32_t *row = g->shadow + (size_t)yy * stride;
        for (int xx = x; xx < x + w; ++xx) {
            if (xx < 0 || (uint32_t)xx >= g->tw) continue;
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
