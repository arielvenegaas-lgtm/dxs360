/* fat32.h: minimal read-only FAT32 for libXenon.
 *
 * libXenon ships no filesystem driver (only ISO9660, ATA-raw and USB mass
 * storage). To load a user-supplied asset from a FAT32 USB stick we parse the
 * volume ourselves on top of a raw sector reader.
 *
 * The sector reader is a callback so the parser stays hardware-independent and
 * can be unit-tested on the host against a real FAT32 image.
 *
 * Limitations (fine for asset loading): read-only, FAT32 only, 512-byte
 * sectors, assumes the first partition starts at LBA 0 (superfloppy) or is the
 * MBR's first FAT32 partition.
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

/* Locates the FAT32 volume (handles an MBR's first partition). Returns 1 on
 * success. */
int Fat32_Mount(Fat32ReadSectors rd, void *ctx);

/* Reads up to `maxlen` bytes of the file at `path` (case-insensitive, '/'
 * separators, long names supported) into `buf`. Returns the number of bytes
 * read, or -1 on error / not found. */
long Fat32_ReadFile(const char *path, void *buf, uint32_t maxlen);

/* Callback for Fat32_ListDir. `name` is the entry name (long name if present,
 * else 8.3); `is_dir` is non-zero for a directory. Return non-zero to keep
 * enumerating, 0 to stop early. */
typedef int (*Fat32DirFn)(void *ctx, const char *name, uint32_t size,
                          int is_dir);

/* Enumerates the directory at `path` ("" or "/" = root). Returns the number of
 * entries listed, or -1 if `path` is not an existing directory. '.' and '..'
 * and volume-label entries are skipped. */
long Fat32_ListDir(const char *path, Fat32DirFn fn, void *ctx);

#ifdef __cplusplus
}
#endif
