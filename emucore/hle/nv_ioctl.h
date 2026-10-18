/* nv_ioctl.h: NVIDIA ioctl request number encoding + the commands this HLE
 * layer decodes.
 *
 * The encoding is not guessed from numeric values: it is the Linux-derived
 * primitive that libnx uses, verified against
 * libnx `nx/include/switch/nvidia/ioctl.h`:
 *
 *     #define _NV_IOC(dir,type,nr,size) \
 *       (((dir) << 30) | ((type) << 8) | ((nr) << 0) | ((size) << 16))
 *
 * i.e. a 32-bit ioctl request packs:
 *     bits  0..7   number  (nr)
 *     bits  8..15  type    (group / magic)
 *     bits 16..29  size    (sizeof the argument struct, max 0x3FFF)
 *     bits 30..31  direction
 *
 * Direction follows the Switch/libnx convention where WRITE is the guest
 * *providing* data to the driver (input) and READ is the driver *returning*
 * data to the guest (output) -- see `nvIoctl` in libnx `services/nv.c`:
 *     if (dir & _NV_IOC_WRITE) { buf_send = argp; ... }
 *     if (dir & _NV_IOC_READ)  { buf_recv = argp; ... }
 */
#ifndef EMUCORE_HLE_NV_IOCTL_H
#define EMUCORE_HLE_NV_IOCTL_H

#include <stdint.h>

#define NV_IOC_NRBITS   8
#define NV_IOC_TYPEBITS 8
#define NV_IOC_SIZEBITS 14
#define NV_IOC_DIRBITS  2

#define NV_IOC_NRSHIFT   0
#define NV_IOC_TYPESHIFT (NV_IOC_NRSHIFT + NV_IOC_NRBITS)
#define NV_IOC_SIZESHIFT (NV_IOC_TYPESHIFT + NV_IOC_TYPEBITS)
#define NV_IOC_DIRSHIFT  (NV_IOC_SIZESHIFT + NV_IOC_SIZEBITS)

#define NV_IOC_NRMASK   ((1u << NV_IOC_NRBITS) - 1u)
#define NV_IOC_TYPEMASK ((1u << NV_IOC_TYPEBITS) - 1u)
#define NV_IOC_SIZEMASK ((1u << NV_IOC_SIZEBITS) - 1u)
#define NV_IOC_DIRMASK  ((1u << NV_IOC_DIRBITS) - 1u)

#define NV_IOC_NONE  0u
#define NV_IOC_WRITE 1u  /* guest -> driver (input)  */
#define NV_IOC_READ  2u  /* driver -> guest (output) */

#define NV_IOC(dir, type, nr, size) \
    (((uint32_t)(dir) << NV_IOC_DIRSHIFT) | \
     ((uint32_t)(type) << NV_IOC_TYPESHIFT) | \
     ((uint32_t)(nr) << NV_IOC_NRSHIFT) | \
     ((uint32_t)(size) << NV_IOC_SIZESHIFT))

#define NV_IOC_DIR(nr)  (((uint32_t)(nr) >> NV_IOC_DIRSHIFT)  & NV_IOC_DIRMASK)
#define NV_IOC_TYPE(nr) (((uint32_t)(nr) >> NV_IOC_TYPESHIFT) & NV_IOC_TYPEMASK)
#define NV_IOC_NR(nr)   (((uint32_t)(nr) >> NV_IOC_NRSHIFT)   & NV_IOC_NRMASK)
#define NV_IOC_SIZE(nr) (((uint32_t)(nr) >> NV_IOC_SIZESHIFT) & NV_IOC_SIZEMASK)

/* Type (group) magic values. */
#define NV_MAGIC_CUSTOM 0x00u /* /dev/nvhost-ctrl, /dev/nvmap, ...        */
#define NV_MAGIC_GPU_AS 0x41u /* /dev/nvhost-as-gpu                       */
#define NV_MAGIC_GPU    0x47u /* /dev/nvhost-ctrl-gpu, /dev/nvhost-gpu    */
#define NV_MAGIC_HOST   0x48u /* channel ioctls                           */

/* /dev/nvhost-ctrl commands (type NV_MAGIC_CUSTOM). Values match
 * switchbrew "NV services" and libnx's device usage. */
#define NVHOST_IOCTL_CTRL_SYNCPT_READ    NV_IOC(NV_IOC_READ,              NV_MAGIC_CUSTOM, 0x14, 8)
#define NVHOST_IOCTL_CTRL_SYNCPT_INCR    NV_IOC(NV_IOC_WRITE,             NV_MAGIC_CUSTOM, 0x15, 4)
#define NVHOST_IOCTL_CTRL_GET_CONFIG     NV_IOC(NV_IOC_READ | NV_IOC_WRITE, NV_MAGIC_CUSTOM, 0x1B, 387)

/* NVHOST_IOCTL_CTRL_GET_CONFIG argument (switchbrew "NV services"):
 *
 *   struct {
 *     __in  char name[0x41];   // domain string, e.g. "nv"
 *     __in  char key[0x41];    // e.g. "NV_MEMORY_PROFILER"
 *     __out char value[0x101]; // configuration string
 *   };
 */
#define NVHOST_CTRL_CONFIG_NAME_OFF   0x00
#define NVHOST_CTRL_CONFIG_KEY_OFF    0x41
#define NVHOST_CTRL_CONFIG_VALUE_OFF  0x82
#define NVHOST_CTRL_CONFIG_NAME_SIZE  0x41
#define NVHOST_CTRL_CONFIG_KEY_SIZE   0x41
#define NVHOST_CTRL_CONFIG_VALUE_SIZE 0x101
#define NVHOST_CTRL_CONFIG_SIZE \
    (NVHOST_CTRL_CONFIG_VALUE_OFF + NVHOST_CTRL_CONFIG_VALUE_SIZE) /* 0x183 */

#endif /* EMUCORE_HLE_NV_IOCTL_H */
