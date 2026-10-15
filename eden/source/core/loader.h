/* loader.h: real, read-only probing of Switch executable-module headers.
 *
 * This is genuine (non-executing) parsing: it decodes the header of an NSO
 * (the format inside retail NCAs) or an NRO (homebrew module) so the frontend
 * can describe the module's real layout. It NEVER maps or runs any code, and
 * every field is bounds-checked against the bytes actually read, so a bad or
 * truncated parse is reported as "invalid" rather than shown as fact.
 *
 * This is the honest first piece of a future loader; it is NOT emulation.
 */
#pragma once

#include <stdint.h>

typedef struct {
    int      valid;          /* header recognised and self-consistent     */
    int      is_nso;         /* 1 = NSO0, 0 = NRO0                        */
    uint32_t version;
    uint32_t flags;
    uint32_t declared_size;  /* total size from the header (NRO NroSize)  */

    int      has_segments;   /* segments decoded AND bounds-checked       */
    uint32_t text_off, text_size;
    uint32_t ro_off,   ro_size;
    uint32_t data_off, data_size;
    uint32_t bss_size;

    int      has_build_id;
    uint8_t  build_id[32];
} ModuleInfo;

/* Probes the first `len` bytes of a module. Returns 1 and fills `out` when the
 * buffer starts with a valid NSO0/NRO0 header, otherwise returns 0 and clears
 * `out`. Pure: performs no I/O and allocates nothing. */
int Loader_ProbeModule(const void *data, uint32_t len, ModuleInfo *out);
