/* library.h: the in-memory game library (frontend milestone).
 *
 * This milestone only *discovers and presents* files. Nothing here boots a
 * game: `supported` is reserved for a future emulation core and is always 0
 * for now, so the UI can honestly distinguish "detected file" from "playable".
 */
#pragma once

#include <stdint.h>

#define GAME_MAX 512

typedef struct {
    char     file[96];   /* basename, e.g. "Hollow Knight.nsp"        */
    char     title[96];  /* display title (basename without extension) */
    char     ext[8];     /* lower-case extension including the dot     */
    uint32_t size;       /* bytes                                      */
    int      supported;  /* 1 once a core can boot it; always 0 today  */
    uint16_t hue;        /* deterministic placeholder colour seed      */
    /* Optional user-supplied box art, loaded lazily by the UI as a .spr. */
    uint32_t *art;
    uint16_t  art_w, art_h;
    int       art_tried;
} Game;

typedef struct {
    Game items[GAME_MAX];
    int  count;        /* games added (known extensions)             */
    int  files_seen;   /* every file encountered during a scan       */
    int  dirs_seen;    /* subdirectories encountered                 */
    int  rejected;     /* files with an unknown extension            */
} Library;

void Library_Init(Library *l);
void Library_Clear(Library *l);

/* Adds `name` (a basename) if it has a known Switch-game extension.
 * Returns 1 if added, 0 if the extension is not recognised. */
int  Library_AddFile(Library *l, const char *name, uint32_t size);

/* True for ".nsp", ".xci", ".nro", ".nso" (case-insensitive). */
int  Library_IsGameExt(const char *ext);

/* Counts items whose `supported` is set. */
int  Library_SupportedCount(const Library *l);
