/* fs.h: USB mass-storage + FAT32 access for the Eden360 frontend.
 *
 * libXenon has no filesystem driver, so this layers the read-only FAT32
 * parser (xenon/source/fat32.c) on top of the raw USB mass-storage sector
 * API. The scan is read-only: games are never written or modified.
 */
#pragma once

#include <stdint.h>

#define FS_PATH_MAX 128

/* Directory callback: return non-zero to keep going, 0 to stop. */
typedef int (*FsDirFn)(void *ctx, const char *name, uint32_t size, int is_dir);

/* Brings up USB and mounts the first FAT32 mass-storage device. `tick` (may be
 * NULL) is called periodically with a human-readable status so the UI can
 * animate; return 0 from `tick` to abort the wait. Returns 1 on success. */
int Fs_Mount(int (*tick)(void *ctx, const char *status), void *ctx);

int Fs_Ready(void);

/* Lists a directory. Returns entries listed, or -1 if it does not exist. */
long Fs_ListDir(const char *path, FsDirFn fn, void *ctx);

/* Reads a whole file into `buf` (up to `max`). Returns bytes read, or -1. */
long Fs_ReadFile(const char *path, void *buf, uint32_t max);

/* The resolved roms/ directory ("" if none was found). */
const char *Fs_RomsDir(void);

/* Short human-readable storage status, e.g. "USB FAT32 ready" or "no USB". */
const char *Fs_Status(void);
