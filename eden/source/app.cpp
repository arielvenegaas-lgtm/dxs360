/* app.cpp: screen logic and library scanning for the Eden360 frontend. */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "ui/layout.h"
#include "fs.h"
#include "core.h"
#include "artwork.h"

static const char *kHome[] = { "Library", "System Status", "Settings", "About", "Rescan Games" };
#define HOME_N ((int)(sizeof(kHome) / sizeof(kHome[0])))

static const char *kMenu[] = { "Library", "System Status", "Settings", "About", "Rescan Games", "Close" };
#define MENU_N ((int)(sizeof(kMenu) / sizeof(kMenu[0])))

static const char *kSet[] = {
    "Grid columns", "Show file extensions", "Scan roms on start",
    "Performance (FPS HUD)", "Teleport destination", "Sort games",
    "Confirm before delete", "Rescan now", "Back"
};
#define SET_N ((int)(sizeof(kSet) / sizeof(kSet[0])))

/* Per-game action menu (opened with A in the library / info screen). */
enum { ACT_PLAY, ACT_INFO, ACT_TELEPORT, ACT_DELETE, ACT_CANCEL, ACT_N };

/* Teleport destination presets (all volume-relative). "HDD" is honoured on
 * machines whose internal drive is a real FAT32 volume; on a stock 360 the
 * drive is FATX and unwritable, so the copy lands on the writable volume. */
static const char *kMoveTargets[] = {
    "switch/games", "switch", "games", "roms", "eden/games"
};
#define MOVE_N ((int)(sizeof(kMoveTargets) / sizeof(kMoveTargets[0])))

const char *App_MoveTargetName(const EdenApp *a) {
    int i = a ? a->set.move_target : 0;
    if (i < 0 || i >= MOVE_N) i = 0;
    return kMoveTargets[i];
}

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

/* Case-insensitive comparison of two titles for sorting. */
static int TitleLess(const char *a, const char *b) {
    for (;;) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return ca < cb;
        if (!ca) return 0;
        ++a; ++b;
    }
}

/* Stable-ish insertion sort (the library is small and swaps stay local). */
void App_SortLibrary(EdenApp *a) {
    int n = a->lib.count;
    if (n < 2) return;
    App_ArtFlush(a); /* indices move: drop the cover cache */
    for (int i = 1; i < n; ++i) {
        Game key = a->lib.items[i];
        int j = i - 1;
        while (j >= 0) {
            Game *cur = &a->lib.items[j];
            int after;
            if (a->set.sort_mode == 1)
                after = cur->size < key.size;
            else
                after = !TitleLess(cur->title[0] ? cur->title : cur->file,
                                   key.title[0] ? key.title : key.file);
            if (!after) break;
            a->lib.items[j + 1] = *cur;
            --j;
        }
        a->lib.items[j + 1] = key;
    }
}

void App_ShowMessage(EdenApp *a, const char *text) {
    SetStr(a->msg, (int)sizeof(a->msg), text ? text : "");
    a->msg_t = 3.5f;
}

/* ---- local cover art (+ small bounded cache) -------------------------- *
 *
 * Covers are ordinary PNG/BMP files sitting next to the game (see
 * game/artwork.h). Decoding is done once per game and cached; the cache is
 * bounded so a large library can never exhaust the console's RAM. Nothing is
 * fetched over the network and no cover is invented.
 */
#define ART_CACHE_MAX 16
#define ART_MAX_W     220
#define ART_MAX_H     330

static Image g_art[ART_CACHE_MAX];
static int   g_art_idx[ART_CACHE_MAX];
static int   g_art_n;

static long ArtFsRead(void *user, const char *path, void *buf, uint32_t max) {
    (void)user;
    return Fs_ReadFile(path, buf, max);
}

void App_ArtFlush(EdenApp *a) {
    (void)a;
    for (int i = 0; i < g_art_n; ++i) Img_Free(&g_art[i]);
    g_art_n = 0;
}

const Image *App_GameArt(EdenApp *a, int idx) {
    if (!a || idx < 0 || idx >= a->lib.count) return NULL;
    if (!a->fs_ready) return NULL;
    for (int i = 0; i < g_art_n; ++i)
        if (g_art_idx[i] == idx) return &g_art[i];

    Game *gm = &a->lib.items[idx];
    if (gm->art_tried) return NULL; /* already looked: no cover next to it */
    gm->art_tried = 1;

    Image img;
    if (!Artwork_LoadForGame(ArtFsRead, NULL, gm->dir, gm->file,
                             ART_MAX_W, ART_MAX_H, &img, NULL, 0))
        return NULL;

    if (g_art_n >= ART_CACHE_MAX) { /* evict oldest */
        int ev = g_art_idx[0];
        Img_Free(&g_art[0]);
        /* The evicted game can be retried if it scrolls back into view. */
        if (ev >= 0 && ev < a->lib.count) a->lib.items[ev].art_tried = 0;
        for (int i = 1; i < ART_CACHE_MAX; ++i) {
            g_art[i - 1] = g_art[i];
            g_art_idx[i - 1] = g_art_idx[i];
        }
        g_art_n = ART_CACHE_MAX - 1;
    }
    g_art[g_art_n] = img;
    g_art_idx[g_art_n] = idx;
    ++g_art_n;
    return &g_art[g_art_n - 1];
}

/* ---- recursive library scan ------------------------------------------ *
 *
 * `Fs_ListDir` enumerates one folder at a time, so the walk is driven from a
 * fixed queue held in BSS (bare-metal stacks are small). Every game folder is
 * recorded relative to the volume root, which is what `Game.dir` stores, so a
 * queued path can never be longer than that field can hold.
 *
 * Nothing here is hardcoded to a folder name: the scan always starts at the
 * volume root and walks downwards, so USB:/roms/, USB:/games/ and any deeper
 * layout are all discovered (fs.h requirement 4).
 */
#define SCAN_DIR_MAX  96   /* == sizeof(Game.dir): queued paths always fit  */
#define SCAN_QMAX     1024 /* directories visited per scan                 */

static char g_scanq[SCAN_QMAX][SCAN_DIR_MAX];
static int  g_scanq_n;
static int  g_scanq_head;

typedef struct {
    EdenApp *a;
    char     dir[SCAN_DIR_MAX]; /* folder being walked, "" = volume root   */
} ScanCtx;

/* Queues a child folder for the outer walk; `c->dir` + "/" + `name`. */
static void ScanQueue(ScanCtx *c, const char *name) {
    if (g_scanq_n >= SCAN_QMAX) return;
    char *out = g_scanq[g_scanq_n];
    int n;
    if (c->dir[0]) n = snprintf(out, SCAN_DIR_MAX, "%s/%s", c->dir, name);
    else           n = snprintf(out, SCAN_DIR_MAX, "%s", name);
    if (n <= 0 || n >= SCAN_DIR_MAX) return; /* too deep: out of Game.dir range */
    ++g_scanq_n;
}

static int ScanCb(void *vctx, const char *name, uint32_t size, int is_dir) {
    ScanCtx *c = (ScanCtx *)vctx;
    if (c->a->lib.count >= GAME_MAX) return 0; /* stop listing: library full */
    if (is_dir) {
        c->a->lib.dirs_seen++;
        ScanQueue(c, name);
        return 1;
    }
    Library_AddFileDir(&c->a->lib, c->dir, name, size);
    return 1;
}

/* Walks the volume root (and everything below it) into `a->lib`. */
static void ScanVolume(EdenApp *a) {
    g_scanq_n = 0;
    g_scanq_head = 0;
    g_scanq[g_scanq_n++][0] = 0; /* seed with the volume root */

    while (g_scanq_head < g_scanq_n) {
        if (a->lib.count >= GAME_MAX) break;

        ScanCtx c;
        c.a = a;
        memcpy(c.dir, g_scanq[g_scanq_head], SCAN_DIR_MAX);
        ++g_scanq_head;

        char path[FS_PATH_MAX];
        if (c.dir[0]) snprintf(path, sizeof(path), "%s", c.dir);
        else          path[0] = 0; /* "" = volume root */

        if (Fs_ListDir(path, ScanCb, &c) < 0) continue;
    }
}

/* Reads the first bytes of a game's container and fills its metadata (real
 * title ID, container type, content count). Metadata only - nothing executes. */
static unsigned char g_metabuf[65536];

static void LoadGameMeta(EdenApp *a, Game *g) {
    if (!g || g->meta_scanned) return;
    g->meta_scanned = 1;
    if (!a->fs_ready) return;

    char path[256];
    if (g->dir[0]) snprintf(path, sizeof(path), "%s/%s", g->dir, g->file);
    else           snprintf(path, sizeof(path), "%s", g->file);
    long n = Fs_ReadFile(path, g_metabuf, (uint32_t)sizeof(g_metabuf));
    if (n <= 0) return;
    Library_ScanContainer(g_metabuf, (uint32_t)n, g);
}

void App_Rescan(EdenApp *a) {
    App_ArtFlush(a);
    Library_Clear(&a->lib);
    a->lib_sel = 0;
    a->lib_page = 0;

    /* Storage is mounted lazily here (never at boot): libXenon's USB
     * mass-storage probe can hang on some consoles when the ELF is launched
     * from Aurora, so we only touch it when the user asks for a scan. */
    if (!a->fs_ready) a->fs_ready = Fs_EnsureMounted();

    if (!a->fs_ready) {
        SetStr(a->status, (int)sizeof(a->status), Fs_Status());
        return;
    }
    /* Walk the whole volume: no particular folder name is required. */
    ScanVolume(a);
    App_SortLibrary(a);

    char buf[96];
    if (a->lib.count > 0) {
        snprintf(buf, sizeof(buf), "%d game%s detected (%d file%s scanned)",
                 a->lib.count, a->lib.count == 1 ? "" : "s",
                 a->lib.files_seen, a->lib.files_seen == 1 ? "" : "s");
    } else if (a->lib.files_seen > 0) {
        snprintf(buf, sizeof(buf), "%d file%s but no .nsp/.xci/.nro/.nso",
                 a->lib.files_seen, a->lib.files_seen == 1 ? "" : "s");
    } else {
        snprintf(buf, sizeof(buf), "USB mounted, no files on the stick");
    }
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
    a->set.fps_hud = 0;
    a->set.sort_mode = 0;
    a->set.move_target = 0;
    a->set.confirm_del = 1;
    Library_Init(&a->lib);
    Nav_Reset();
    a->screen = SCR_HOME;
    a->back_to = SCR_HOME;
    a->fs_ready = Fs_Ready();
    SetStr(a->status, (int)sizeof(a->status), "Ready");

    /* Bring up the emulation core (guest RAM + interpreter + loaders) and run
     * its deterministic self-test once at boot, so the status line reports the
     * real result instead of "not run". Cheap: a few dozen instructions. */
    Core_Init();
}

static void ToggleInt(int *v) { *v = *v ? 0 : 1; }

/* Volume-relative path of a game (dir/file). */
static void GamePath(const Game *g, char *out, int outsz) {
    if (g->dir[0]) snprintf(out, outsz, "%s/%s", g->dir, g->file);
    else           snprintf(out, outsz, "%s", g->file);
}

/* Opens the per-game action menu for item `idx`. */
static void OpenActions(EdenApp *a, int idx) {
    if (a->lib.count <= 0) return;
    if (idx < 0 || idx >= a->lib.count) return;
    a->act_idx = idx;
    a->lib_sel = idx;
    a->detail_idx = idx;
    LoadGameMeta(a, &a->lib.items[idx]);
    a->act_sel = 0;
    a->act_confirm = 0;
    a->screen = SCR_ACTIONS;
}

/* Play: try to actually run the selection through the emucore bridge. The
 * message it returns is always truthful (standalone NSO runs; a retail NSP
 * reports exactly what is still missing, e.g. NCA decryption). */
static void RunGame(EdenApp *a, int idx) {
    char bootmsg[224];
    Core_Boot(&a->lib.items[idx], bootmsg, (int)sizeof(bootmsg));
    App_ShowMessage(a, bootmsg[0] ? bootmsg : Core_WhyNot(&a->lib.items[idx]));
}

/* Teleport: copy the game into the destination folder (switch/games/ by
 * default) and remove the source so it is a real move. The internal Xbox 360
 * HDD is FATX and cannot be written, so the copy lands on the writable volume
 * (USB) and the chosen folder lives on it. */
static void ActionTeleport(EdenApp *a) {
    Game *g = &a->lib.items[a->act_idx];
    char src[256], dst[256], msg[224];
    GamePath(g, src, sizeof(src));

    int dstv = Fs_VolumeByType("HDD");
    if (dstv < 0 || !Fs_VolumeWritable(dstv)) dstv = 0;
    if (!Fs_VolumeWritable(dstv)) {
        App_ShowMessage(a, "No writable volume for Teleport.");
        return;
    }

    const char *tgt = App_MoveTargetName(a);
    snprintf(dst, sizeof(dst), "%s/%s", tgt, g->file);
    if (strcmp(src, dst) == 0) {
        snprintf(msg, sizeof(msg), "Already in %s/", tgt);
        App_ShowMessage(a, msg);
        return;
    }
    Fs_MkdirsOn(dstv, tgt);

    uint64_t bytes = 0;
    if (!Fs_CopyFileOn(dstv, dst, 0, src, 0, 0, &bytes)) {
        snprintf(msg, sizeof(msg), "Teleport failed (copy to %s/)", tgt);
        App_ShowMessage(a, msg);
        return;
    }
    int removed = Fs_UnlinkOn(0, src); /* best-effort: complete the move */
    App_Rescan(a);
    snprintf(msg, sizeof(msg), "Teleported to %s/ (%.1f MB)%s", tgt,
             (double)bytes / (1024.0 * 1024.0),
             removed ? "" : "; source kept");
    App_ShowMessage(a, msg);
}

/* Delete: unlink the game from the volume, then rescan. */
static void ActionDelete(EdenApp *a) {
    Game *g = &a->lib.items[a->act_idx];
    char src[256], name[96], msg[224];
    GamePath(g, src, sizeof(src));
    SetStr(name, (int)sizeof(name), g->file); /* capture before App_Rescan */

    if (Fs_UnlinkOn(0, src)) {
        App_Rescan(a);
        snprintf(msg, sizeof(msg), "Deleted %s", name);
    } else {
        snprintf(msg, sizeof(msg), "Delete failed: %s", src);
    }
    App_ShowMessage(a, msg);
}

static void ActivateAction(EdenApp *a, int idx) {
    switch (idx) {
    case ACT_PLAY:     RunGame(a, a->act_idx); a->screen = SCR_LIBRARY; break;
    case ACT_INFO:     a->detail_idx = a->act_idx; a->screen = SCR_DETAIL; break;
    case ACT_TELEPORT: ActionTeleport(a); a->screen = SCR_LIBRARY; break;
    case ACT_DELETE:   ActionDelete(a); a->screen = SCR_LIBRARY; break;
    default:           a->screen = SCR_LIBRARY; break; /* Cancel */
    }
}

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

    /* Smoothed FPS for the performance HUD. */
    a->fps_accum += dt;
    ++a->fps_frames;
    if (a->fps_accum >= 0.5f) {
        a->fps = (float)a->fps_frames / a->fps_accum;
        a->fps_accum = 0.0f;
        a->fps_frames = 0;
    }

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
        if (nav.accept) OpenActions(a, a->lib_sel);
        if (nav.back)   a->screen = SCR_HOME;
        break;
    }

    case SCR_DETAIL: {
        int n = a->lib.count;
        if (nav.back) { a->screen = SCR_LIBRARY; break; }
        if (n > 0) {
            if (nav.lb) { a->lib_sel = (a->lib_sel + n - 1) % n; a->detail_idx = a->lib_sel; LoadGameMeta(a, &a->lib.items[a->detail_idx]); }
            if (nav.rb) { a->lib_sel = (a->lib_sel + 1) % n; a->detail_idx = a->lib_sel; LoadGameMeta(a, &a->lib.items[a->detail_idx]); }
            if (nav.accept) OpenActions(a, a->detail_idx);
        }
        break;
    }

    case SCR_ACTIONS: {
        if (a->act_confirm) {
            /* Destructive-action confirmation: A = yes, B = no. */
            if (nav.accept) { a->act_confirm = 0; ActionDelete(a); a->screen = SCR_LIBRARY; }
            else if (nav.back || nav.down || nav.up) a->act_confirm = 0;
            break;
        }
        if (nav.up)   a->act_sel = (a->act_sel + ACT_N - 1) % ACT_N;
        if (nav.down) a->act_sel = (a->act_sel + 1) % ACT_N;
        if (nav.back) { a->screen = SCR_LIBRARY; break; }
        if (nav.accept) {
            if (a->act_sel == ACT_DELETE && a->set.confirm_del) { a->act_confirm = 1; break; }
            ActivateAction(a, a->act_sel);
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
            if (nav.accept || nav.left || nav.right) ToggleInt(&a->set.fps_hud);
        } else if (a->set_sel == 4) {
            if (nav.right || nav.accept) { a->set.move_target = (a->set.move_target + 1) % MOVE_N; }
            if (nav.left)  { a->set.move_target = (a->set.move_target + MOVE_N - 1) % MOVE_N; }
        } else if (a->set_sel == 5) {
            if (nav.accept || nav.left || nav.right) {
                a->set.sort_mode = a->set.sort_mode ? 0 : 1;
                App_SortLibrary(a);
                a->lib_sel = 0;
                a->lib_page = 0;
            }
        } else if (a->set_sel == 6) {
            if (nav.accept || nav.left || nav.right) ToggleInt(&a->set.confirm_del);
        } else if (a->set_sel == 7) {
            if (nav.accept) { App_Rescan(a); App_ShowMessage(a, a->status); }
        } else if (a->set_sel == 8) {
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
