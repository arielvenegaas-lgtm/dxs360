/* fat32.h: minimal FAT32 for libXenon (read + write).
 *
 * libXenon ships no filesystem driver (only ISO9660, ATA-raw and USB mass
 * storage). To load - and now also store - user-supplied assets on a FAT32
 * volume we parse the volume ourselves on top of a raw sector reader/writer.
 *
 * The sector callbacks keep the parser hardware-independent so it can be
 * unit-tested on the host against a real FAT32 image (mkfs.fat + mtools).
 *
 * Two ways to use it:
 *   - Fat32_Open()/Fat32_MountVolume() + the Fat32V_* instance calls: several
 *     volumes can be mounted at once (e.g. USB + HDD). This is what the
 *     frontend uses.
 *   - The legacy Fat32_Mount()/Fat32_ReadFile()/Fat32_ListDir() globals: a thin
 *     wrapper over one default volume, kept for the game/PoC builds.
 *
 * Limitations: 512-byte sectors, FAT32 only. Reads handle a superfloppy (VBR at
 * LBA 0) or an MBR's FAT32 partition (primary + extended/EBR chain). Writes
 * require an explicit write callback; on a read-only volume the Fat32V_* write
 * calls return 0.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reads `count` 512-byte sectors starting at `lba` into `buf`. Returns 1 on
 * success, 0 on failure. */
typedef int (*Fat32ReadSectors)(void *ctx, uint32_t lba, uint32_t count,
                                void *buf);

/* Writes `count` 512-byte sectors starting at `lba` from `buf`. Returns 1 on
 * success, 0 on failure. May be NULL for a read-only volume. */
typedef int (*Fat32WriteSectors)(void *ctx, uint32_t lba, uint32_t count,
                                 const void *buf);

/* Callback for Fat32_ListDir. `name` is the entry name (long name if present,
 * else 8.3); `is_dir` is non-zero for a directory. Return non-zero to keep
 * enumerating, 0 to stop early. */
typedef int (*Fat32DirFn)(void *ctx, const char *name, uint32_t size,
                          int is_dir);

/* Recognised on-disk filesystems when probing a volume. */
typedef enum {
    FAT32_FS_NONE = 0,
    FAT32_FS_FAT32,     /* supported (read + write)             */
    FAT32_FS_FATX,      /* Xbox 360 HDD filesystem (NOT supported) */
    FAT32_FS_FAT16,     /* recognised but unsupported           */
    FAT32_FS_UNKNOWN
} Fat32FsType;

/* A mounted (or mountable) volume. */
typedef struct Fat32 Fat32;

/* Creates an empty volume object. `wr` may be NULL for read-only. */
Fat32 *Fat32_Open(Fat32ReadSectors rd, Fat32WriteSectors wr, void *ctx);
void   Fat32_Close(Fat32 *fs);

/* Probes the filesystem at LBA 0 / the first partition without mounting.
 * Lets the caller tell a FATX Xbox HDD apart from a FAT32 volume. */
Fat32FsType Fat32_Probe(Fat32ReadSectors rd, void *ctx);

/* Locates and mounts the FAT32 volume. Returns 1 on success. */
int Fat32_MountVolume(Fat32 *fs);

int      Fat32_IsWritable(const Fat32 *fs);
uint64_t Fat32_TotalBytes(const Fat32 *fs);
/* Free space in bytes (uses the cached free-cluster count computed at mount). */
uint64_t Fat32_FreeBytes(const Fat32 *fs);

/* --- directory / file operations (instance based) --- */

/* Lists `path` ("" or "/" = root). Returns entry count, or -1 if not a dir. */
long Fat32V_ListDir(Fat32 *fs, const char *path, Fat32DirFn fn, void *ctx);

/* Reads up to `maxlen` bytes of a file. Returns bytes read or -1. */
long Fat32V_ReadFile(Fat32 *fs, const char *path, void *buf, uint32_t maxlen);

/* Stats `path`. Either out pointer may be NULL. Returns 1 if it exists. */
int  Fat32V_Stat(Fat32 *fs, const char *path, uint32_t *out_size, int *out_isdir);

/* Returns 1 if `path` exists. */
int  Fat32V_Exists(Fat32 *fs, const char *path);

/* Creates a directory (like "mkdir -p" for the final component; parent must
 * exist). Returns 1 on success, 0 on failure / read-only. */
int  Fat32V_Mkdir(Fat32 *fs, const char *path);

/* Deletes a regular file and frees its clusters. Returns 1 on success. */
int  Fat32V_Unlink(Fat32 *fs, const char *path);

/* Writes a whole in-memory buffer to `path` (create/truncate). Intended for
 * small files; games are copied with the streaming writer below. */
int  Fat32V_WriteFile(Fat32 *fs, const char *path, const void *data, uint32_t len);

/* Streaming file writer for large copies (games). */
typedef struct Fat32File Fat32File;
Fat32File *Fat32File_Create(Fat32 *fs, const char *path);
int  Fat32File_Write(Fat32File *f, const void *data, uint32_t len);
int  Fat32File_Finish(Fat32File *f);   /* flushes + sets size; frees handle */
void Fat32File_Abort(Fat32File *f);    /* frees clusters + entry; frees handle */

/* Streaming file reader for large files (games): reads sequentially without
 * loading the whole file into RAM. */
typedef struct Fat32Reader Fat32Reader;
Fat32Reader *Fat32Reader_Open(Fat32 *fs, const char *path, uint32_t *out_size);
/* Reads up to `len` bytes; returns bytes read (0 at EOF), or -1 on error. */
long Fat32Reader_Read(Fat32Reader *r, void *buf, uint32_t len);
/* Repositions to absolute byte offset `off` (walks the FAT chain, not data
 * sectors). Returns 1 on success, 0 if `off` is past EOF. */
int  Fat32Reader_Seek(Fat32Reader *r, uint32_t off);
void Fat32Reader_Close(Fat32Reader *r);

/* --- legacy global API (single default volume) --- */

/* Locates the FAT32 volume (handles an MBR's first partition). Returns 1 on
 * success. */
int Fat32_Mount(Fat32ReadSectors rd, void *ctx);

/* Reads up to `maxlen` bytes of the file at `path` into `buf`. Returns bytes
 * read, or -1. */
long Fat32_ReadFile(const char *path, void *buf, uint32_t maxlen);

/* Enumerates `path` ("" or "/" = root). Returns entry count or -1. */
long Fat32_ListDir(const char *path, Fat32DirFn fn, void *ctx);

#ifdef __cplusplus
}
#endif
