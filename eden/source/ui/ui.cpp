/* ui.cpp: all screen drawing for the Eden360 frontend.
 *
 * Original work. Visual style (dark slate + sky-blue accent) is inspired by
 * Eden's default_dark theme, but no Eden code or assets are used.
 */
#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "ui.h"
#include "layout.h"
#include "theme.h"
#include "core.h"
#include "fs.h"

#define GLYPH_W 6 /* built-in 5x7 font advance, in font pixels */

/* ---- primitives ------------------------------------------------------- */

static int U(const EdenApp *a) {
    int u = ((int)a->H + 360) / 720;
    return u < 1 ? 1 : u;
}

static void Fill(GfxContext *g, int x, int y, int w, int h, uint32_t c) {
    if (w > 0 && h > 0) Gfx_FillRect(g, x, y, w, h, c);
}

static void Frame(GfxContext *g, int x, int y, int w, int h, int t, uint32_t c) {
    Fill(g, x, y, w, t, c);
    Fill(g, x, y + h - t, w, t, c);
    Fill(g, x, y, t, h, c);
    Fill(g, x + w - t, y, t, h, c);
}

static int TextW(const char *s, int sc) {
    return (int)strlen(s) * GLYPH_W * sc;
}

static void TextC(GfxContext *g, int cx, int y, int sc, uint32_t c, const char *s) {
    Gfx_DrawText(g, cx - TextW(s, sc) / 2, y, sc, c, s);
}

static void TextClip(GfxContext *g, int x, int y, int sc, int maxw, uint32_t c,
                     const char *s) {
    int maxc = maxw / (GLYPH_W * sc);
    if (maxc < 1) maxc = 1;
    char tmp[160];
    int n = 0;
    int sl = (int)strlen(s);
    if (sl <= maxc) {
        for (; s[n] && n < (int)sizeof(tmp) - 1; ++n) tmp[n] = s[n];
    } else {
        int keep = maxc - 2;
        if (keep < 0) keep = 0;
        for (; n < keep; ++n) tmp[n] = s[n];
        tmp[n++] = '.';
        tmp[n++] = '.';
    }
    tmp[n] = 0;
    Gfx_DrawText(g, x, y, sc, c, tmp);
}

static uint32_t Hsv(int h, float s, float v) {
    h = ((h % 360) + 360) % 360;
    float f = (h % 60) / 60.0f;
    float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    float r, gg, b;
    switch (h / 60) {
    case 0:  r = v; gg = t; b = p; break;
    case 1:  r = q; gg = v; b = p; break;
    case 2:  r = p; gg = v; b = t; break;
    case 3:  r = p; gg = q; b = v; break;
    case 4:  r = t; gg = p; b = v; break;
    default: r = v; gg = p; b = q; break;
    }
    return 0xFF000000u | ((uint32_t)(r * 255) << 16) |
           ((uint32_t)(gg * 255) << 8) | (uint32_t)(b * 255);
}

static void FmtSize(char *d, int sz, uint32_t b) {
    if (b >= 1024u * 1024u * 1024u)      snprintf(d, sz, "%.2f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024u * 1024u)          snprintf(d, sz, "%.1f MB", b / (1024.0 * 1024));
    else if (b >= 1024u)                  snprintf(d, sz, "%.0f KB", b / 1024.0);
    else                                  snprintf(d, sz, "%u B", (unsigned)b);
}

/* First 8 bytes of a build id as lowercase hex (16 chars). */
static void BuildIdStr(char *d, int sz, const uint8_t *id) {
    int n = 0;
    for (int i = 0; i < 8 && n < sz - 1; ++i)
        n += snprintf(d + n, sz - n, "%02x", (unsigned)id[i]);
}

static uint32_t LerpCol(uint32_t c0, uint32_t c1, int t, int n) {
    if (n < 1) n = 1;
    int r0 = (c0 >> 16) & 0xFF, g0 = (c0 >> 8) & 0xFF, b0 = c0 & 0xFF;
    int r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    int r = r0 + (r1 - r0) * t / n;
    int gg = g0 + (g1 - g0) * t / n;
    int b = b0 + (b1 - b0) * t / n;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gg << 8) | (uint32_t)b;
}

/* Vertical gradient drawn as horizontal bands (Gfx_FillRect is opaque). */
static void VGradient(GfxContext *g, int x, int y, int w, int h,
                      uint32_t c0, uint32_t c1) {
    int bands = 48;
    if (h < bands) bands = h;
    if (bands < 1) return;
    for (int i = 0; i < bands; ++i) {
        int yy = y + h * i / bands;
        int yh = y + h * (i + 1) / bands - yy;
        Fill(g, x, yy, w, yh, LerpCol(c0, c1, i, bands));
    }
}

/* Draws text with a 1-pixel (scaled) black drop shadow for legibility. */
static void TextS(GfxContext *g, int x, int y, int sc, uint32_t c, const char *s) {
    Gfx_DrawText(g, x + sc, y + sc, sc, COL_BLACK, s);
    Gfx_DrawText(g, x, y, sc, c, s);
}

static const char *TextOr(const char *s) { return (s && s[0]) ? s : "-"; }

/* Label/value row with the value clipped to the remaining width. */
static void Row(GfxContext *g, int x, int y, int u, int valx, int maxw,
                const char *label, const char *val) {
    Gfx_DrawText(g, x, y, 2 * u, COL_MUTED, label);
    TextClip(g, valx, y, 2 * u, maxw, COL_TEXT, val);
}

static const char *GameTitle(const EdenApp *a, const Game *g) {
    return a->set.show_ext ? g->file : g->title;
}

/* ---- chrome ----------------------------------------------------------- */

static void Backdrop(GfxContext *g, const EdenApp *a) {
    int W = (int)a->W, H = (int)a->H;

    /* Deep slate gradient: darker at the top, easing into the page colour. */
    VGradient(g, 0, 0, W, H, 0xFF0C1721u, COL_BG);

    /* Soft sky-blue glow band under the header. */
    for (int i = 0; i < 6; ++i)
        Fill(g, 0, i * 7, W, 7, LerpCol(0xFF223D52u, 0xFF0C1721u, i, 6));

    /* Faint grid for a little depth. */
    uint32_t grid = 0xFF17222Du;
    for (int x = 0; x < W; x += 96) Fill(g, x, 0, 1, H, grid);
    for (int y = 0; y < H; y += 96) Fill(g, 0, y, W, 1, grid);
}

static void Header(GfxContext *g, const EdenApp *a, const char *title) {
    int u = U(a);
    Fill(g, 0, 0, (int)a->W, UI_HEADER_H, COL_PANEL);
    Fill(g, 0, UI_HEADER_H - 2, (int)a->W, 2, COL_ACCENT);

    TextS(g, UI_MARGIN, (UI_HEADER_H - 5 * 3 * u) / 2, 3 * u, COL_ACCENT, EDEN_APPNAME);

    int lx = UI_MARGIN + TextW(EDEN_APPNAME, 3 * u) + 16 * u;
    TextS(g, lx, (UI_HEADER_H - 2 * 2 * u) / 2, 2 * u, COL_TEXT_DIM, title);

    const char *ver = EDEN_EDITION "  v" EDEN_VERSION;
    Gfx_DrawText(g, (int)a->W - UI_MARGIN - TextW(ver, 2 * u),
                 (UI_HEADER_H - 2 * 2 * u) / 2, 2 * u, COL_MUTED, ver);
}

static const char *ScreenHint(int s) {
    switch (s) {
    case SCR_HOME:     return "A Select   Start Menu";
    case SCR_LIBRARY:  return "A Open   B Back   LB/RB Page   Start Menu";
    case SCR_DETAIL:   return "LB/RB Next game   A Why not?   B Back";
    case SCR_SETTINGS: return "A/Left/Right Change   B Back";
    case SCR_SYSTEM:   return "A Back   B Back";
    case SCR_ABOUT:    return "A Back   B Back";
    case SCR_MENU:     return "A Select   B Close";
    default:           return "";
    }
}

static void Footer(GfxContext *g, const EdenApp *a) {
    int u = U(a);
    int y = (int)a->H - UI_FOOTER_H;
    Fill(g, 0, y, (int)a->W, UI_FOOTER_H, COL_PANEL);
    Fill(g, 0, y, (int)a->W, 2, COL_LINE);

    /* Left: current status. Middle: control hints for the active screen. */
    Gfx_DrawText(g, UI_MARGIN, y + (UI_FOOTER_H - 2 * u) / 2, u, COL_TEXT_DIM, a->status);

    const char *hint = ScreenHint(a->screen);
    if (hint[0]) {
        Gfx_DrawText(g, (int)a->W / 2 - TextW(hint, u) / 2,
                     y + (UI_FOOTER_H - 2 * u) / 2, u, COL_MUTED, hint);
    }

    char right[64];
    snprintf(right, sizeof(right), "%d games    Core: %s", a->lib.count, Core_BackendName());
    Gfx_DrawText(g, (int)a->W - UI_MARGIN - TextW(right, u),
                 y + (UI_FOOTER_H - 2 * u) / 2, u, COL_MUTED, right);
}

/* ---- box art ---------------------------------------------------------- */

/* Format + region badges drawn over the art (placeholder or real cover). */
static void ArtBadges(GfxContext *g, const EdenApp *a, const Game *gm,
                      int x, int y, int w, int h) {
    int u = U(a);
    const char *ext = (gm->ext[0] == '.') ? gm->ext + 1 : gm->ext;
    int es = u;
    Fill(g, x + w - TextW(ext, es) - 10 * es, y + h - 7 * es - 8 * es,
         TextW(ext, es) + 8 * es, 7 * es + 6 * es, 0xB0000000u);
    Gfx_DrawText(g, x + w - TextW(ext, es) - 6 * es, y + h - 7 * es - 6 * es,
                 es, COL_TEXT, ext);

    /* Region badge, top-left, when the filename carried one. */
    if (gm->region[0]) {
        int rs = u;
        Fill(g, x + 8 * rs, y + 8 * rs, TextW(gm->region, rs) + 8 * rs,
             7 * rs + 6 * rs, 0xB0000000u);
        Gfx_DrawText(g, x + 12 * rs, y + 8 * rs + 3 * rs, rs, COL_ACCENT, gm->region);
    }
}

static void PlaceholderArt(GfxContext *g, const Game *gm,
                           int x, int y, int w, int h) {
    uint32_t base = Hsv(gm->hue, 0.42f, 0.42f);
    uint32_t top  = Hsv(gm->hue, 0.55f, 0.62f);
    uint32_t bot  = Hsv(gm->hue, 0.60f, 0.22f);

    Fill(g, x, y, w, h, base);
    Fill(g, x, y, w, h / 2, top);
    Fill(g, x, y + h / 2, w, h - h / 2, bot);

    char initial[2] = { gm->title[0] ? gm->title[0] : '?', 0 };
    int sc = h / 3 / 7;
    if (sc < 1) sc = 1;
    TextC(g, x + w / 2, y + h / 2 - 7 * sc / 2, sc, COL_WHITE, initial);
}

/* Draws a game's cover: the local cover image when one was found next to the
 * file, otherwise the deterministic coloured placeholder. */
static void GameArt(GfxContext *g, EdenApp *a, const Game *gm, int idx,
                    int x, int y, int w, int h) {
    const Image *im = App_GameArt(a, idx);
    if (im && im->pixels)
        Gfx_DrawImage(g, x, y, w, h, im->pixels, (int)im->w, (int)im->h);
    else
        PlaceholderArt(g, gm, x, y, w, h);
    ArtBadges(g, a, gm, x, y, w, h);
}

/* ---- screens ---------------------------------------------------------- */

static void DrawHome(GfxContext *g, EdenApp *a) {
    int u = U(a);
    int bodyY = UI_HEADER_H;

    TextS(g, UI_MARGIN, bodyY + 60 * u, 7 * u, COL_TEXT, EDEN_APPNAME);
    Gfx_DrawText(g, UI_MARGIN, bodyY + 60 * u + 7 * 7 * u + 8 * u, 2 * u,
                 COL_ACCENT, EDEN_EDITION " - game library");

    int itemH = 58 * u;
    int menuY = bodyY + 220 * u;
    static const char *items[] = { "Library", "System Status", "Settings", "About", "Rescan Games" };
    int n = 5;
    for (int i = 0; i < n; ++i) {
        int y = menuY + i * itemH;
        int sel = (i == a->home_sel);
        if (sel) {
            Fill(g, UI_MARGIN - 14 * u, y, 8 * u, itemH - 10 * u, COL_ACCENT);
            Fill(g, UI_MARGIN, y, (int)a->W / 2 - UI_MARGIN, itemH - 10 * u, COL_PANEL_2);
        }
        Gfx_DrawText(g, UI_MARGIN + 16 * u, y + (itemH - 10 * u - 3 * 2 * u) / 2,
                     3 * u, sel ? COL_TEXT : COL_TEXT_DIM, items[i]);
    }

    /* Info card on the right. */
    int cx = (int)a->W / 2 + UI_MARGIN;
    int cw = (int)a->W - UI_MARGIN - cx;
    int cy = bodyY + 60 * u;
    int ch = (int)a->H - UI_FOOTER_H - 60 * u - cy;
    Fill(g, cx, cy, cw, ch, COL_PANEL);
    Frame(g, cx, cy, cw, ch, 1, COL_LINE);

    int px = cx + 28 * u;
    int py = cy + 26 * u;
    Gfx_DrawText(g, px, py, 2 * u, COL_ACCENT, "GAME LIBRARY");

    char num[24];
    snprintf(num, sizeof(num), "%d", a->lib.count);
    Gfx_DrawText(g, px, py + 26 * u, 9 * u, COL_TEXT, num);
    Gfx_DrawText(g, px + TextW(num, 9 * u) + 14 * u, py + 26 * u + 6 * 7 * u,
                 2 * u, COL_TEXT_DIM, a->lib.count == 1 ? "game detected" : "games detected");

    int ly = py + 26 * u + 9 * 7 * u + 24 * u;
    Fill(g, px, ly, cw - 56 * u, 1, COL_LINE);
    ly += 18 * u;

    char line[128];
    snprintf(line, sizeof(line), "Folder:  %s", Fs_RomsDir()[0] ? Fs_RomsDir() : "(none)");
    Gfx_DrawText(g, px, ly, 2 * u, COL_TEXT_DIM, line); ly += 30 * u;
    snprintf(line, sizeof(line), "Storage: %s", Fs_Status());
    Gfx_DrawText(g, px, ly, 2 * u, COL_TEXT_DIM, line); ly += 30 * u;
    snprintf(line, sizeof(line), "Device:  %s", Fs_DeviceType()[0] ? Fs_DeviceType() : "(none)");
    Gfx_DrawText(g, px, ly, 2 * u, COL_TEXT_DIM, line); ly += 30 * u;
    snprintf(line, sizeof(line), "Keys:    %s", Fs_KeysStatus());
    Gfx_DrawText(g, px, ly, 2 * u, COL_TEXT_DIM, line); ly += 30 * u;
    if (Fs_LastDiag()[0]) {
        snprintf(line, sizeof(line), "Diag:    %s", Fs_LastDiag());
        Gfx_DrawText(g, px, ly, 2 * u, COL_TEXT_DIM, line); ly += 30 * u;
    }
    if (Fs_RootSummary()[0]) {
        snprintf(line, sizeof(line), "Root:    %s", Fs_RootSummary());
        Gfx_DrawText(g, px, ly, 2 * u, COL_TEXT_DIM, line); ly += 30 * u;
    }
    snprintf(line, sizeof(line), "Files seen: %d   Supported: %d",
             a->lib.files_seen, Library_SupportedCount(&a->lib));
    Gfx_DrawText(g, px, ly, 2 * u, COL_TEXT_DIM, line); ly += 30 * u;
    {
        char vol[24];
        uint64_t vb = Fs_VolumeSize();
        if (vb) snprintf(vol, sizeof(vol), "%.1f GB", (double)vb / 1073741824.0);
        else    snprintf(vol, sizeof(vol), "unknown");
        snprintf(line, sizeof(line), "Devices: %d   Volume: %s   Games: %d",
                 Fs_DeviceCount(), vol, a->lib.count);
    }
    Gfx_DrawText(g, px, ly, 2 * u, COL_TEXT_DIM, line); ly += 30 * u;

    Gfx_DrawText(g, px, ly, 2 * u, COL_WARN, "Frontend only - no game can boot yet.");
}

static void DrawEmpty(GfxContext *g, EdenApp *a, int bodyY, int bodyH) {
    int u = U(a);
    int cx = (int)a->W / 2;
    int y = bodyY + bodyH / 2 - 120 * u;

    TextC(g, cx, y, 5 * u, COL_ACCENT, "No games found");
    y += 5 * 7 * u + 20 * u;
    TextC(g, cx, y, 2 * u, COL_TEXT, "Put .nsp / .xci / .nro / .nso files into the roms folder");
    y += 2 * 7 * u + 10 * u;
    char p[128];
    snprintf(p, sizeof(p), "on the USB stick%s%s",
             Fs_RomsDir()[0] ? " (" : "", Fs_RomsDir()[0] ? Fs_RomsDir() : "");
    strncat(p, Fs_RomsDir()[0] ? ")" : "", sizeof(p) - strlen(p) - 1);
    TextC(g, cx, y, 2 * u, COL_TEXT_DIM, p);
    y += 2 * 7 * u + 24 * u;
    TextC(g, cx, y, 2 * u, COL_TEXT_DIM, "Then press Start > Rescan Games to refresh.");
    y += 2 * 7 * u + 16 * u;
    TextC(g, cx, y, 2 * u, COL_ACCENT, "Press A now to scan USB storage.");
    y += 2 * 7 * u + 16 * u;
    TextC(g, cx, y, 2 * u, COL_MUTED, Fs_Status());
}

static void DrawLibrary(GfxContext *g, EdenApp *a) {
    int u = U(a);
    int bodyY = UI_HEADER_H;
    int bodyH = (int)a->H - UI_FOOTER_H - UI_HEADER_H;

    if (a->lib.count == 0) { DrawEmpty(g, a, bodyY, bodyH); return; }

    int cols = a->set.grid_cols;
    int gridW = (int)a->W - 2 * UI_MARGIN;
    int tileW = (gridW - (cols - 1) * UI_GAP) / cols;
    int artH = tileW * UI_ART_RATIO_N / UI_ART_RATIO_D;
    int tileH = artH + UI_TILE_TITLE_H;
    int per = App_LibPerPage(a);
    int start = a->lib_page * per;

    int x0 = UI_MARGIN;
    int y0 = bodyY + UI_MARGIN - 8 * u;

    for (int i = 0; i < per; ++i) {
        int idx = start + i;
        if (idx >= a->lib.count) break;
        int col = i % cols, row = i / cols;
        int x = x0 + col * (tileW + UI_GAP);
        int y = y0 + row * (tileH + UI_GAP);
        const Game *gm = &a->lib.items[idx];
        int sel = (idx == a->lib_sel);

        if (sel) Frame(g, x - 3, y - 3, tileW + 6, artH + 6, 3, COL_ACCENT);
        else     Frame(g, x - 1, y - 1, tileW + 2, artH + 2, 1, COL_LINE);

        GameArt(g, a, gm, idx, x, y, tileW, artH);

        TextClip(g, x, y + artH + 8 * u, 2 * u, tileW, sel ? COL_TEXT : COL_TEXT_DIM,
                 GameTitle(a, gm));

        char sz[24];
        FmtSize(sz, sizeof(sz), gm->size);
        Gfx_DrawText(g, x, y + artH + 8 * u + 2 * 7 * u + 4 * u, u, COL_MUTED, sz);

        if (!gm->supported) {
            int bw = TextW("DETECTED", u) + 10 * u;
            Fill(g, x + tileW - bw, y + artH + 8 * u + 2 * 7 * u + 2 * u, bw, 7 * u + 6 * u,
                 COL_PANEL);
            Gfx_DrawText(g, x + tileW - bw + 5 * u, y + artH + 8 * u + 2 * 7 * u + 5 * u,
                         u, COL_WARN, "DETECTED");
        }
    }

    /* page indicator */
    char pg[48];
    int pages = (a->lib.count + per - 1) / per;
    snprintf(pg, sizeof(pg), "Page %d / %d", a->lib_page + 1, pages);
    Gfx_DrawText(g, (int)a->W - UI_MARGIN - TextW(pg, 2 * u),
                 bodyY + 30 * u, 2 * u, COL_TEXT_DIM, pg);
}

static void DrawDetail(GfxContext *g, EdenApp *a) {
    int u = U(a);
    if (a->lib.count == 0) { DrawEmpty(g, a, UI_HEADER_H, (int)a->H - UI_FOOTER_H - UI_HEADER_H); return; }
    const Game *gm = &a->lib.items[a->detail_idx];

    int bodyY = UI_HEADER_H + UI_MARGIN;
    int artW = (int)a->W * 34 / 100;
    int artH = artW * UI_ART_RATIO_N / UI_ART_RATIO_D;
    if (artH > (int)a->H - UI_FOOTER_H - bodyY - UI_MARGIN)
        artH = (int)a->H - UI_FOOTER_H - bodyY - UI_MARGIN;
    Frame(g, UI_MARGIN - 2, bodyY - 2, artW + 4, artH + 4, 2, COL_LINE);
    GameArt(g, a, gm, a->detail_idx, UI_MARGIN, bodyY, artW, artH);

    int tx = UI_MARGIN + artW + 40 * u;
    int ty = bodyY;
    TextS(g, tx, ty, 4 * u, COL_TEXT, gm->title);
    ty += 4 * 7 * u + 6 * u;
    {
        /* Show where the file actually lives on the stick. */
        char fpath[200];
        if (gm->dir[0]) snprintf(fpath, sizeof(fpath), "%s/%s", gm->dir, gm->file);
        else            snprintf(fpath, sizeof(fpath), "%s", gm->file);
        Gfx_DrawText(g, tx, ty, 2 * u, COL_TEXT_DIM, fpath);
    }
    ty += 2 * 7 * u + 20 * u;

    char sz[24];
    FmtSize(sz, sizeof(sz), gm->size);
    char cnt[32];
    if (gm->content_count > 0)
        snprintf(cnt, sizeof(cnt), "%d file%s", gm->content_count,
                 gm->content_count == 1 ? "" : "s");
    else
        snprintf(cnt, sizeof(cnt), "-");

    const char *ext = (gm->ext[0] == '.') ? gm->ext + 1 : (gm->ext[0] ? gm->ext : "?");

    int valx = tx + 168 * u;
    int maxw = (int)a->W - valx - UI_MARGIN;
    int rh = 2 * 7 * u + 6 * u;
    Row(g, tx, ty, u, valx, maxw, "Title ID",  TextOr(gm->title_id)); ty += rh;
    Row(g, tx, ty, u, valx, maxw, "Version",   TextOr(gm->version));  ty += rh;
    Row(g, tx, ty, u, valx, maxw, "Region",    TextOr(gm->region));   ty += rh;
    Row(g, tx, ty, u, valx, maxw, "Container", Library_ContainerName(gm->container)); ty += rh;
    Row(g, tx, ty, u, valx, maxw, "Contents",  cnt);                  ty += rh;
    Row(g, tx, ty, u, valx, maxw, "Size",      sz);                   ty += rh;
    Row(g, tx, ty, u, valx, maxw, "Type",      ext);                  ty += rh;
    Row(g, tx, ty, u, valx, maxw, "Device",
        Fs_DeviceType()[0] ? Fs_DeviceType() : "-");                  ty += rh;

    /* Real module layout, decoded by core/loader (NSO/NRO only). */
    if (gm->mod.valid) {
        if (gm->mod.has_segments) {
            char tbuf[24], rbuf[24], dbuf[24], bbuf[24], seg[128];
            FmtSize(tbuf, sizeof(tbuf), gm->mod.text_size);
            FmtSize(rbuf, sizeof(rbuf), gm->mod.ro_size);
            FmtSize(dbuf, sizeof(dbuf), gm->mod.data_size);
            FmtSize(bbuf, sizeof(bbuf), gm->mod.bss_size);
            snprintf(seg, sizeof(seg), "text %s  ro %s  data %s  bss %s",
                     tbuf, rbuf, dbuf, bbuf);
            Row(g, tx, ty, u, valx, maxw,
                gm->mod.is_nso ? "NSO layout" : "NRO layout", seg);   ty += rh;
        }
        if (gm->mod.has_build_id) {
            char bid[24];
            BuildIdStr(bid, sizeof(bid), gm->mod.build_id);
            Row(g, tx, ty, u, valx, maxw, "Build ID", bid);           ty += rh;
        }
        if (gm->mod.version) {
            char vv[16];
            snprintf(vv, sizeof(vv), "%u", (unsigned)gm->mod.version);
            Row(g, tx, ty, u, valx, maxw, "Module ver", vv);          ty += rh;
        }
    }
    ty += 12 * u;

    Gfx_DrawText(g, tx, ty, 2 * u, COL_WARN, "Status: DETECTED (not bootable)");
    ty += 2 * 7 * u + 16 * u;

    int bw = (int)a->W - tx - UI_MARGIN;
    int bh = 2 * 7 * u * 3 + 40 * u;
    if (ty + bh < (int)a->H - UI_FOOTER_H - UI_MARGIN) {
        Fill(g, tx, ty, bw, bh, COL_PANEL);
        Frame(g, tx, ty, bw, bh, 1, COL_LINE);
        Gfx_DrawText(g, tx + 16 * u, ty + 14 * u, 2 * u, COL_ACCENT, "What does this mean?");
        Gfx_DrawText(g, tx + 16 * u, ty + 14 * u + 2 * 7 * u + 8 * u, 2 * u, COL_TEXT_DIM,
                     "Eden360 read this file's header to show its real title ID");
        Gfx_DrawText(g, tx + 16 * u, ty + 14 * u + 2 * (2 * 7 * u) + 12 * u, 2 * u, COL_TEXT_DIM,
                     "and contents. Booting games is not implemented in this build.");
    }
}

static void DrawSettings(GfxContext *g, EdenApp *a) {
    int u = U(a);
    int x = UI_MARGIN;
    int y = UI_HEADER_H + 60 * u;
    int w = (int)a->W - 2 * UI_MARGIN;

    Gfx_DrawText(g, x, y, 4 * u, COL_TEXT, "Settings"); y += 4 * 7 * u + 30 * u;

    static const char *labels[] = {
        "Grid columns", "Show file extensions", "Scan roms on start", "Rescan now", "Back"
    };
    int itemH = 60 * u;
    for (int i = 0; i < 5; ++i) {
        int iy = y + i * itemH;
        int sel = (i == a->set_sel);
        if (sel) {
            Fill(g, x - 12 * u, iy, 8 * u, itemH - 12 * u, COL_ACCENT);
            Fill(g, x, iy, w, itemH - 12 * u, COL_PANEL_2);
        }
        Gfx_DrawText(g, x + 18 * u, iy + (itemH - 12 * u - 2 * 7 * u) / 2, 2 * u,
                     sel ? COL_TEXT : COL_TEXT_DIM, labels[i]);

        const char *val = "";
        char vbuf[16];
        if (i == 0) { snprintf(vbuf, sizeof(vbuf), "%d", a->set.grid_cols); val = vbuf; }
        else if (i == 1) val = a->set.show_ext ? "On" : "Off";
        else if (i == 2) val = a->set.scan_on_start ? "On" : "Off";

        if (val[0]) {
            int vx = x + w - 24 * u - TextW(val, 2 * u);
            Gfx_DrawText(g, vx, iy + (itemH - 12 * u - 2 * 7 * u) / 2, 2 * u,
                         sel ? COL_ACCENT : COL_TEXT_DIM, val);
        }
    }
}

static void DrawAbout(GfxContext *g, EdenApp *a) {
    int u = U(a);
    int x = UI_MARGIN;
    int y = UI_HEADER_H + 50 * u;
    int w = (int)a->W - 2 * UI_MARGIN;

    Gfx_DrawText(g, x, y, 6 * u, COL_ACCENT, EDEN_APPNAME); y += 6 * 7 * u + 6 * u;
    Gfx_DrawText(g, x, y, 2 * u, COL_TEXT, EDEN_EDITION); y += 2 * 7 * u + 6 * u;
    Gfx_DrawText(g, x, y, 2 * u, COL_TEXT_DIM, "Version " EDEN_VERSION); y += 2 * 7 * u + 24 * u;

    Fill(g, x, y, w, 1, COL_LINE); y += 20 * u;

    static const char *lines[] = {
        "A Nintendo Switch game-library frontend for the Xbox 360.",
        "Free and open source (GPL-3.0-or-later).",
        "Inspired by the Eden emulator project; not affiliated with, nor",
        "endorsed by, Eden, Nintendo, or Microsoft.",
        "",
        "It reads each file's header to show its real title ID, region and",
        "contents. This build is a FRONTEND ONLY: it does NOT emulate the",
        "Nintendo Switch and cannot boot any game.",
        "",
        "Built on libXenon / free60. No proprietary Xbox 360 SDK is used.",
    };
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i) {
        Gfx_DrawText(g, x, y, 2 * u, COL_TEXT_DIM, lines[i]);
        y += 2 * 7 * u + 7 * u;
    }

    y += 8 * u;
    Gfx_DrawText(g, x, y, 2 * u, COL_ACCENT, Fs_KeysStatus());
    y += 2 * 7 * u + 7 * u;
    Gfx_DrawText(g, x, y, 2 * u, COL_MUTED, "keys are not used by this frontend build");
}

static void DrawSystem(GfxContext *g, EdenApp *a) {
    int u = U(a);
    int x = UI_MARGIN;
    int w = (int)a->W - 2 * UI_MARGIN;
    int y = UI_HEADER_H + 30 * u;

    Gfx_DrawText(g, x, y, 4 * u, COL_TEXT, "System Status"); y += 4 * 7 * u + 8 * u;
    Gfx_DrawText(g, x, y, 2 * u, COL_TEXT_DIM, Core_StatusLine());
    y += 2 * 7 * u + 16 * u;
    Fill(g, x, y, w, 1, COL_LINE); y += 14 * u;

    /* ---- capabilities ------------------------------------------------- */
    Gfx_DrawText(g, x, y, 2 * u, COL_ACCENT, "CAPABILITIES"); y += 2 * 7 * u + 10 * u;
    for (int i = 0; i < Core_CapabilityCount(); ++i) {
        const CoreCapability *c = Core_CapabilityAt(i);
        uint32_t sc = (c->state == CAPSTATE_NONE) ? COL_BAD
                    : (c->state == CAPSTATE_READY) ? COL_GOOD : COL_WARN;
        Gfx_DrawText(g, x, y, 2 * u, COL_TEXT, c->name);
        int sx = x + 130 * u;
        Gfx_DrawText(g, sx, y, 2 * u, sc, Core_CapStateName(c->state));
        TextClip(g, sx + 150 * u, y, 2 * u, w - 150 * u - (sx - x) - 40 * u,
                 COL_TEXT_DIM, c->detail);
        y += 2 * 7 * u + 6 * u;
    }
    y += 8 * u;
    Fill(g, x, y, w, 1, COL_LINE); y += 14 * u;

    /* ---- requirements ------------------------------------------------- */
    Gfx_DrawText(g, x, y, 2 * u, COL_ACCENT, "WHAT A SWITCH TITLE NEEDS vs WHAT THIS CONSOLE HAS");
    y += 2 * 7 * u + 10 * u;

    const int cx1 = x + 150 * u;
    const int cx2 = x + 470 * u;
    const int cx3 = x + 810 * u;
    Gfx_DrawText(g, x,     y, u, COL_MUTED, "RESOURCE");
    Gfx_DrawText(g, cx1,   y, u, COL_MUTED, "SWITCH");
    Gfx_DrawText(g, cx2,   y, u, COL_MUTED, "XBOX 360");
    Gfx_DrawText(g, cx3,   y, u, COL_MUTED, "FEASIBLE");
    y += 7 * u + 8 * u;

    for (int i = 0; i < Core_RequirementCount(); ++i) {
        const CoreRequirement *r = Core_RequirementAt(i);
        Gfx_DrawText(g, x,   y, u, COL_TEXT_DIM, r->name);
        Gfx_DrawText(g, cx1, y, u, COL_TEXT_DIM, r->guest);
        Gfx_DrawText(g, cx2, y, u, COL_TEXT_DIM, r->host);
        Gfx_DrawText(g, cx3, y, u, r->feasible ? COL_GOOD : COL_BAD,
                     r->feasible ? "yes" : "no");
        y += 7 * u + 8 * u;
    }

    y += 6 * u;
    Fill(g, x, y, w, 1, COL_LINE); y += 14 * u;

    /* ---- verdict ------------------------------------------------------ */
    Fill(g, x, y, w, 2 * 7 * u + 24 * u, COL_PANEL);
    Frame(g, x, y, w, 2 * 7 * u + 24 * u, 1, COL_LINE);
    Gfx_DrawText(g, x + 16 * u, y + 12 * u, 2 * u, COL_WARN, Core_Feasibility());
    y += 2 * 7 * u + 34 * u;
    Gfx_DrawText(g, x, y, u, COL_MUTED, "Nothing on this screen is emulated - it only reports status.");
}

static void DrawMenuOverlay(GfxContext *g, EdenApp *a) {
    int u = U(a);
    int w = 460 * u, h = 430 * u;
    int x = (int)a->W - w - UI_MARGIN;
    int y = UI_HEADER_H + 20 * u;
    Fill(g, x, y, w, h, COL_PANEL);
    Frame(g, x, y, w, h, 2, COL_ACCENT);

    Gfx_DrawText(g, x + 24 * u, y + 20 * u, 3 * u, COL_ACCENT, "Menu");
    int iy = y + 20 * u + 3 * 7 * u + 22 * u;
    int itemH = 52 * u;
    static const char *items[] = { "Library", "System Status", "Settings", "About", "Rescan Games", "Close" };
    for (int i = 0; i < 6; ++i) {
        int yy = iy + i * itemH;
        int sel = (i == a->menu_sel);
        if (sel) Fill(g, x + 16 * u, yy, w - 32 * u, itemH - 10 * u, COL_PANEL_2);
        Gfx_DrawText(g, x + 32 * u, yy + (itemH - 10 * u - 2 * 7 * u) / 2, 2 * u,
                     sel ? COL_TEXT : COL_TEXT_DIM, items[i]);
    }
}

static void DrawMessage(GfxContext *g, EdenApp *a) {
    if (a->msg_t <= 0.0f || !a->msg[0]) return;
    int u = U(a);
    int w = (int)a->W * 60 / 100;
    int h = 90 * u;
    int x = ((int)a->W - w) / 2;
    int y = (int)a->H - UI_FOOTER_H - h - 24 * u;
    Fill(g, x, y, w, h, COL_PANEL_2);
    Frame(g, x, y, w, h, 2, COL_ACCENT);
    TextClip(g, x + 24 * u, y + (h - 2 * 7 * u) / 2, 2 * u, w - 48 * u, COL_TEXT, a->msg);
}

void UI_Draw(EdenApp *a) {
    GfxContext *g = a->gfx;
    Backdrop(g, a);

    switch (a->screen) {
    case SCR_HOME:     Header(g, a, "Home");     DrawHome(g, a); break;
    case SCR_LIBRARY:  Header(g, a, "Game Library"); DrawLibrary(g, a); break;
    case SCR_DETAIL:   Header(g, a, "Game Info"); DrawDetail(g, a); break;
    case SCR_SETTINGS: Header(g, a, "Settings"); DrawSettings(g, a); break;
    case SCR_SYSTEM:   Header(g, a, "System Status"); DrawSystem(g, a); break;
    case SCR_ABOUT:    Header(g, a, "About");    DrawAbout(g, a); break;
    case SCR_MENU:     Header(g, a, "Menu");     DrawMenuOverlay(g, a); break;
    }

    if (a->screen != SCR_MENU && a->screen != SCR_ABOUT) DrawMessage(g, a);
    Footer(g, a);
}

void UI_DrawLoading(EdenApp *a, const char *status, float t) {
    GfxContext *g = a->gfx;
    int u = U(a);
    Backdrop(g, a);

    int cx = (int)a->W / 2;
    int cy = (int)a->H / 2;

    TextC(g, cx, cy - 120 * u, 9 * u, COL_ACCENT, EDEN_APPNAME);
    TextC(g, cx, cy - 120 * u + 9 * 7 * u + 10 * u, 2 * u, COL_TEXT_DIM, EDEN_EDITION);
    TextC(g, cx, cy + 20 * u, 2 * u, COL_TEXT, status ? status : "");

    /* simple animated dots */
    int n = ((int)(t * 3.0f)) % 4;
    for (int i = 0; i < 4; ++i) {
        int s = 14 * u;
        int x = cx - (4 * (s + 10 * u)) / 2 + i * (s + 10 * u);
        Fill(g, x, cy + 70 * u, s, s, (i == n) ? COL_ACCENT : COL_LINE);
    }

    TextC(g, cx, (int)a->H - UI_FOOTER_H - 40 * u, u, COL_MUTED,
          "libXenon / free60 - frontend build");
}
