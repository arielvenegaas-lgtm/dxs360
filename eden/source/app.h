/* app.h: application state and screen logic for the Eden360 frontend. */
#pragma once

#include <stdint.h>

#include "gfx.h"
#include "input.h"
#include "nav.h"
#include "library.h"

#define EDEN_APPNAME  "Eden"
#define EDEN_EDITION  "Xbox 360 Frontend"
#define EDEN_VERSION  "0.3.0"

typedef enum {
    SCR_HOME,
    SCR_LIBRARY,
    SCR_DETAIL,
    SCR_SETTINGS,
    SCR_ABOUT,
    SCR_SYSTEM,
    SCR_MENU      /* Start overlay */
} Screen;

typedef struct {
    int grid_cols;     /* 3..5 */
    int show_ext;      /* append the file extension to titles */
    int scan_on_start; /* scan roms/ at boot */
} Settings;

typedef struct {
    GfxContext *gfx;
    uint32_t    W, H;

    Screen screen;
    Screen back_to;    /* screen to return to from the Start overlay */

    Settings set;
    Library  lib;
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
