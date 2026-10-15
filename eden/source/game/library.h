/* library.h: the in-memory game library (frontend milestone).
 *
 * This milestone only *discovers and presents* files. Nothing here boots a
 * game: `supported` is reserved for a future emulation core and is always 0
 * for now, so the UI can honestly distinguish "detected file" from "playable".
 *
 * Besides the filename, Eden reads a little of each container's header to show
 * correct information (real title ID, container type, content count). This is
 * pure metadata parsing - no game code is executed.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "loader.h" /* core/loader.h: real, read-only module header probe */

#define GAME_MAX 512

#define GAME_TITLEID_LEN 20
#define GAME_VERSION_LEN 10
#define GAME_REGION_LEN  12

/* Container formats Eden can recognise from the file header. */
typedef enum {
    CONT_UNKNOWN = 0,
    CONT_NSP,   /* "PFS0" (also .nsp = PFS0 archive of NCAs)      */
    CONT_XCI,   /* "HEAD" at 0x100 (gamecard image)               */
    CONT_NRO,   /* "NRO0"                                          */
    CONT_NSO     /* "NSO0"                                          */
} Container;

typedef struct {
    char     file[96];   /* basename, e.g. "Cave Story+ [..].nsp"      */
    char     dir[96];    /* relative directory under roms (e.g. "games") or "" */
    char     title[96];  /* display title (clean, without scene tags)  */
    char     ext[8];     /* lower-case extension including the dot     */
    uint32_t size;       /* bytes                                      */
    int      supported;  /* 1 once a core can boot it; always 0 today  */

    /* Metadata parsed from the filename and/or the file header. */
    char     title_id[GAME_TITLEID_LEN]; /* "0100B7D0022EE000" or ""    */
    char     version[GAME_VERSION_LEN];  /* "v0" or ""                  */
    char     region[GAME_REGION_LEN];    /* "US" / "EU" / "WORLD" or "" */
    int      container;                  /* Container enum              */
    int      content_count;              /* entries found in container  */
    int      meta_scanned;               /* header has been read        */
    int      title_id_from_header;       /* title id came from the file */

    ModuleInfo mod;      /* real NSO/NRO module layout (valid==0 if none) */

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

/* Adds a file with relative directory `dir` under the roms root. */
int  Library_AddFileDir(Library *l, const char *dir, const char *name, uint32_t size);

/* True for ".nsp", ".xci", ".nro", ".nso" (case-insensitive). */
int  Library_IsGameExt(const char *ext);

/* Counts items whose `supported` is set. */
int  Library_SupportedCount(const Library *l);

/* Fills g->title / title_id / version / region from a scene-style filename
 * such as "Cave Story+ [0100B7D0022EE000][v0][US].nsp". Pure (no I/O). */
void Library_ParseName(const char *name, Game *g);

/* Inspects the first bytes of a container already read into `data` (len bytes)
 * and fills g->container, g->content_count and (for NSP) g->title_id when it
 * can. Returns the Container value. Pure (no I/O). */
int  Library_ScanContainer(const void *data, uint32_t len, Game *g);

/* Human-readable container name, e.g. "NSP (PFS0)". */
const char *Library_ContainerName(int container);
