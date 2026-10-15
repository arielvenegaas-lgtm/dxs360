/* app.cpp: screen logic and library scanning for the Eden360 frontend. */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "ui/layout.h"
#include "fs.h"
#include "core.h"

static const char *kHome[] = { "Library", "System Status", "Settings", "About", "Rescan Games" };
#define HOME_N ((int)(sizeof(kHome) / sizeof(kHome[0])))

static const char *kMenu[] = { "Library", "System Status", "Settings", "About", "Rescan Games", "Close" };
#define MENU_N ((int)(sizeof(kMenu) / sizeof(kMenu[0])))

static const char *kSet[] = {
    "Grid columns", "Show file extensions", "Scan roms on start", "Rescan now", "Back"
};
#define SET_N ((int)(sizeof(kSet) / sizeof(kSet[0])))

static void SetStr(char *dst, int sz, const char *s) {
    int i = 0;
    for (; s[i] && i < sz - 1; ++i) dst[i] = s[i];
    dst[i] = 0;
}

int App_LibRows(const EdenApp *a) {
    int cols = a->set.grid_cols;
    if (cols < 1) cols = 1;
    int gridW = (int)a->W - 2 * UI_MARGIN;
    int tileW = (gridW - (cols - 1) * UI_GAP) / cols;
    if (tileW < 1) tileW = 1;
    int artH = tileW * UI_ART_RATIO_N / UI_ART_RATIO_D;
    int tileH = artH + UI_TILE_TITLE_H;
    int availH = (int)a->H - UI_HEADER_H - UI_FOOTER_H - 2 * UI_MARGIN;
    int rows = (availH + UI_GAP) / (tileH + UI_GAP);
    if (rows < 1) rows = 1;
    return rows;
}

int App_LibPerPage(const EdenApp *a) {
    int cols = a->set.grid_cols;
    if (cols < 1) cols = 1;
    return cols * App_LibRows(a);
}

void App_ShowMessage(EdenApp *a, const char *text) {
    SetStr(a->msg, (int)sizeof(a->msg), text ? text : "");
    a->msg_t = 3.5f;
}

static int ScanCb(void *ctx, const char *name, uint32_t size, int is_dir) {
    EdenApp *a = (EdenApp *)ctx;
    if (is_dir) { a->lib.dirs_seen++; return 1; }
    Library_AddFile(&a->lib, name, size);
    return 1;
}

/* Reads the first bytes of a game's container and fills its metadata (real
 * title ID, container type, content count). Metadata only - nothing executes. */
static unsigned char g_metabuf[65536];

static void LoadGameMeta(EdenApp *a, Game *g) {
    if (!g || g->meta_scanned) return;
    g->meta_scanned = 1;
    if (!a->fs_ready) return;

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", Fs_RomsDir(), g->file);
    long n = Fs_ReadFile(path, g_metabuf, (uint32_t)sizeof(g_metabuf));
    if (n <= 0) return;
    Library_ScanContainer(g_metabuf, (uint32_t)n, g);
}

void App_Rescan(EdenApp *a) {
    Library_Clear(&a->lib);
    a->lib_sel = 0;
    a->lib_page = 0;

    /* Storage is mounted lazily here (never at boot): libXenon's USB
     * mass-storage probe can hang on some consoles when the ELF is launched
     * from Aurora, so we only touch it when the user asks for a scan. */
    if (!a->fs_ready) a->fs_ready = Fs_EnsureMounted();

    if (!a->fs_ready) {
        SetStr(a->status, (int)sizeof(a->status),
               "No USB storage mounted - connect a FAT32 stick with a roms/ folder");
        return;
    }
    const char *roms = Fs_RomsDir();
    if (!roms || !roms[0]) {
        SetStr(a->status, (int)sizeof(a->status),
               "roms/ folder not found on the USB stick");
        return;
    }

    Fs_ListDir(roms, ScanCb, a);

    char buf[96];
    snprintf(buf, sizeof(buf), "Scanned %s - %d game%s detected", roms,
             a->lib.count, a->lib.count == 1 ? "" : "s");
    SetStr(a->status, (int)sizeof(a->status), buf);
}

void App_Init(EdenApp *a, GfxContext *gfx) {
    memset(a, 0, sizeof(*a));
    a->gfx = gfx;
    a->W = Gfx_Width(gfx);
    a->H = Gfx_Height(gfx);
    a->set.grid_cols = 4;
    a->set.show_ext = 0;
    a->set.scan_on_start = 1;
    Library_Init(&a->lib);
    Nav_Reset();
    a->screen = SCR_HOME;
    a->back_to = SCR_HOME;
    a->fs_ready = Fs_Ready();
    SetStr(a->status, (int)sizeof(a->status), "Ready");
}

static void ToggleInt(int *v) { *v = *v ? 0 : 1; }

static void ActivateMenuItem(EdenApp *a, int idx) {
    switch (idx) {
    case 0: a->screen = SCR_LIBRARY; break;
    case 1: a->screen = SCR_SYSTEM; break;
    case 2: a->set_sel = 0; a->screen = SCR_SETTINGS; break;
    case 3: a->screen = SCR_ABOUT; break;
    case 4: App_Rescan(a); App_ShowMessage(a, a->status); break;
    case 5: a->screen = a->back_to; break; /* Close */
    default: break;
    }
}

void App_Update(EdenApp *a, const PadState *pad, float dt) {
    a->time += dt;
    if (a->msg_t > 0.0f) a->msg_t -= dt;

    Nav nav;
    memset(&nav, 0, sizeof(nav));
    if (pad) { Nav_Update(pad, dt, &nav); }
    a->nav = nav;

    /* Start opens/closes the overlay menu from anywhere. */
    if (nav.start) {
        if (a->screen != SCR_MENU) { a->back_to = a->screen; a->screen = SCR_MENU; a->menu_sel = 0; }
        else { a->screen = a->back_to; }
        return;
    }

    switch (a->screen) {
    case SCR_HOME:
        if (nav.up)   a->home_sel = (a->home_sel + HOME_N - 1) % HOME_N;
        if (nav.down) a->home_sel = (a->home_sel + 1) % HOME_N;
        if (nav.accept) ActivateMenuItem(a, a->home_sel);
        break;

    case SCR_LIBRARY: {
        int n = a->lib.count;
        if (n <= 0) {
            if (nav.back) a->screen = SCR_HOME;
            /* Storage is touched only here, on an explicit press. */
            if (nav.accept) { App_Rescan(a); App_ShowMessage(a, a->status); }
            break;
        }
        if (nav.left)  a->lib_sel = (a->lib_sel + n - 1) % n;
        if (nav.right) a->lib_sel = (a->lib_sel + 1) % n;
        int cols = a->set.grid_cols;
        if (nav.up)    a->lib_sel = (a->lib_sel + n - cols) % n;
        if (nav.down)  a->lib_sel = (a->lib_sel + cols) % n;
        int per = App_LibPerPage(a);
        if (per < 1) per = 1;
        if (nav.lb) a->lib_sel = (a->lib_sel + n - per) % n;
        if (nav.rb) a->lib_sel = (a->lib_sel + per) % n;
        a->lib_page = a->lib_sel / per;
        if (nav.accept) {
            a->detail_idx = a->lib_sel;
            if (a->lib.count > 0) LoadGameMeta(a, &a->lib.items[a->detail_idx]);
            a->screen = SCR_DETAIL;
        }
        if (nav.back)   a->screen = SCR_HOME;
        break;
    }

    case SCR_DETAIL: {
        int n = a->lib.count;
        if (nav.back) { a->screen = SCR_LIBRARY; break; }
        if (n > 0) {
            if (nav.lb) { a->lib_sel = (a->lib_sel + n - 1) % n; a->detail_idx = a->lib_sel; LoadGameMeta(a, &a->lib.items[a->detail_idx]); }
            if (nav.rb) { a->lib_sel = (a->lib_sel + 1) % n; a->detail_idx = a->lib_sel; LoadGameMeta(a, &a->lib.items[a->detail_idx]); }
            if (nav.accept) App_ShowMessage(a, Core_WhyNot(&a->lib.items[a->detail_idx]));
        }
        break;
    }

    case SCR_SETTINGS:
        if (nav.up)   a->set_sel = (a->set_sel + SET_N - 1) % SET_N;
        if (nav.down) a->set_sel = (a->set_sel + 1) % SET_N;
        if (a->set_sel == 0) {
            if (nav.right || nav.accept) { a->set.grid_cols++; if (a->set.grid_cols > 5) a->set.grid_cols = 3; }
            if (nav.left)  { a->set.grid_cols--; if (a->set.grid_cols < 3) a->set.grid_cols = 5; }
        } else if (a->set_sel == 1) {
            if (nav.accept || nav.left || nav.right) ToggleInt(&a->set.show_ext);
        } else if (a->set_sel == 2) {
            if (nav.accept || nav.left || nav.right) ToggleInt(&a->set.scan_on_start);
        } else if (a->set_sel == 3) {
            if (nav.accept) { App_Rescan(a); App_ShowMessage(a, a->status); }
        } else if (a->set_sel == 4) {
            if (nav.accept) a->screen = SCR_HOME;
        }
        if (nav.back) a->screen = SCR_HOME;
        break;

    case SCR_ABOUT:
        if (nav.back || nav.accept) a->screen = SCR_HOME;
        break;

    case SCR_SYSTEM:
        if (nav.back || nav.accept) a->screen = SCR_HOME;
        break;

    case SCR_MENU:
        if (nav.up)   a->menu_sel = (a->menu_sel + MENU_N - 1) % MENU_N;
        if (nav.down) a->menu_sel = (a->menu_sel + 1) % MENU_N;
        if (nav.back) { a->screen = a->back_to; break; }
        if (nav.accept) {
            int sel = a->menu_sel;
            a->screen = a->back_to;
            ActivateMenuItem(a, sel);
        }
        break;
    }
}
