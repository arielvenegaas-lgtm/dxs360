/* loader.h: load an NSO image into guest memory and report the entry point. */
#pragma once

#include <stdint.h>
#include "nso.h"
#include "a64_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LoadedImage {
    uint64_t entry;      /* guest entry address */
    uint64_t image_base; /* lowest mapped address */
    uint64_t image_end;  /* one past the highest mapped byte */
    uint64_t mapped;     /* total bytes written into guest memory */
} LoadedImage;

typedef enum {
    LOAD_OK = 0,
    LOAD_ERR_NSO,
    LOAD_ERR_RANGE,
    LOAD_ERR_DECOMPRESS,
    LOAD_ERR_MEM,
} LoadResult;

const char *Load_ResultName(LoadResult r);

/* Map/decompress all three NSO segments into `mem` (bounds-checked). `nso_data`
 * must point at the raw NSO bytes whose size is `nso_size`. */
LoadResult Nso_LoadIntoGuest(const Nso *n, const void *nso_data, uint64_t nso_size,
                             A64GuestMem *mem, LoadedImage *out);

#ifdef __cplusplus
}
#endif
