/* app.h: application state and screen logic for the Eden360 frontend. */
#pragma once

#include <stdint.h>

#include "gfx.h"
#include "input.h"
#include "nav.h"
#include "library.h"
#include "fs.h"
#include "img.h"

#define EDEN_APPNAME  "Eden"
#define EDEN_EDITION  "Xbox 360 Frontend"
#define EDEN_VERSION  "0.4.0-beta"

typedef enum {
    SCR_HOME,
    SCR_LIBRARY,
    SCR_DETAIL,
    SCR_ACTIONS,  /* per-game action menu (Play / Info / Teleport / Delete) */
    SCR_SETTINGS,
    SCR_ABOUT,
    SCR_SYSTEM,
    SCR_MENU      /* Start overlay */
} Screen;

typedef struct {
    int grid_cols;     /* 3..5 */
    int show_ext;      /* append the file extension to titles */
    int scan_on_start; /* scan roms/ at boot */
    int fps_hud;       /* draw a performance/FPS overlay */
    int sort_mode;     /* 0 = by name, 1 = by size */
    int move_target;   /* index into the Teleport destination presets */
    int confirm_del;   /* ask before deleting a game */
} Settings;

typedef struct {
    GfxContext *gfx;
    uint32_t    W, H;

    Screen screen;
    Screen back_to;    /* screen to return to from the Start overlay */

    Settings set;
    Library  lib;

    int act_idx;       /* game the action menu is acting on */
    int act_sel;       /* selected action */
    int act_confirm;   /* 1 while awaiting a destructive-action confirm */

    float fps;         /* smoothed frames per second (for the HUD) */
    float fps_accum;
    int   fps_frames;
    Nav      nav;

    int home_sel;
    int lib_sel;
    int lib_page;
    int detail_idx;
    int set_sel;
    int menu_sel;

    int  fs_ready;
    char status[96];
    char msg[128];
    float msg_t;
    char last_dir[FS_PATH_MAX];
    char last_file[128];
    int last_count;
    int last_files_seen;
    float time;        /* seconds since start, for animations */
} EdenApp;

void App_Init(EdenApp *a, GfxContext *gfx);
void App_Rescan(EdenApp *a);
void App_Update(EdenApp *a, const PadState *pad, float dt);
void App_ShowMessage(EdenApp *a, const char *text);

/* Library grid geometry (shared with the UI). */
int App_LibRows(const EdenApp *a);
int App_LibPerPage(const EdenApp *a);

/* Local cover art for game `idx`, loaded and cached on first call. Returns
 * NULL when the game ships no readable PNG/BMP cover (the caller then draws its
 * placeholder). Never fetches anything: purely local files next to the game. */
const Image *App_GameArt(EdenApp *a, int idx);

/* Frees every cached cover (called on rescan / shutdown). */
void App_ArtFlush(EdenApp *a);

/* Human-readable name of the current Teleport destination preset. */
const char *App_MoveTargetName(const EdenApp *a);

/* Sorts a->lib in place according to a->set.sort_mode and redraws the cache. */
void App_SortLibrary(EdenApp *a);
