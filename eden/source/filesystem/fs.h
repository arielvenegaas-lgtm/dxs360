/* fs.h: USB mass-storage + FAT32 access for the Eden360 frontend.
 *
 * libXenon has no filesystem driver, so this layers the read-only FAT32
 * parser (xenon/source/fat32.c) on top of the raw USB mass-storage sector
 * API. The scan is read-only: games are never written or modified.
 */
#pragma once

#include <stdint.h>

#define FS_PATH_MAX 128
#define FS_MAX_ROOTS 16

/* Directory callback: return non-zero to keep going, 0 to stop. */
typedef int (*FsDirFn)(void *ctx, const char *name, uint32_t size, int is_dir);

/* Brings up USB and mounts the first FAT32 mass-storage device. `tick` (may be
 * NULL) is called periodically with a human-readable status so the UI can
 * animate; return 0 from `tick` to abort the wait. Returns 1 on success. */
int Fs_Mount(int (*tick)(void *ctx, const char *status), void *ctx);

/* Mounts on first call if not already mounted (no tick callback). Returns the
 * ready flag. Kept separate from Fs_Mount so storage is only touched when the
 * user actually asks for it: libXenon's USB mass-storage probe can hang on some
 * consoles when the ELF is launched from Aurora, so it must not run at boot. */
int Fs_EnsureMounted(void);

int Fs_Ready(void);

/* Lists a directory. Returns entries listed, or -1 if it does not exist. */
long Fs_ListDir(const char *path, FsDirFn fn, void *ctx);

/* Reads a whole file into `buf` (up to `max`). Returns bytes read, or -1. */
long Fs_ReadFile(const char *path, void *buf, uint32_t max);

/* The resolved roms/ directory ("" if none was found). */
const char *Fs_RomsDir(void);

/* Short human-readable storage status, e.g. "USB FAT32 ready" or "no USB". */
const char *Fs_Status(void);

/* Returns the number of detected mass-storage devices (>=0). */
int Fs_DeviceCount(void);

/* Returns total volume size in bytes, or 0 if unknown. */
uint64_t Fs_VolumeSize(void);

/* Counts "name = value" entries in a prod.keys file found under keys/ (0 if
 * the file exists but holds no usable entries, -1 if none was found). Purely
 * informational: this frontend build does not use keys. */
int Fs_KeyCount(void);

/* Human-readable keys status, e.g. "prod.keys: 173 keys" or "prod.keys: none". */
const char *Fs_KeysStatus(void);

/* Gets the list of configured scan roots (volume-relative). Returns count. */
int Fs_GetScanRoots(const char **roots, int max_roots);

/* Returns the device type/name mounted (e.g. "USB" or "HDD") or "" if none. */
const char *Fs_DeviceType(void);

/* Returns last error/diagnostic message for the last operation ("" if none). */
const char *Fs_LastDiag(void);
