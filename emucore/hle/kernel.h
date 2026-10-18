/* kernel.h: Horizon OS kernel-call (SVC) HLE handlers.
 *
 * Real Switch user code issues `svc #N` with N the syscall number and
 * arguments in X0..X7; the result is left in X0. This module implements the
 * subset of kernel calls that a booting runtime (rtld, then the SDK) actually
 * needs, over a caller-supplied memory map. No call pretends to succeed: an
 * unimplemented number is left unhandled so the interpreter halts and the stop
 * is reported by number.
 *
 * The memory map is described as a flat list of regions (the modules' mapped
 * segments plus the process stack). QueryMemory answers from it.
 */
#pragma once

#include <stdint.h>
#include "hle.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HLEK_MAX_REGIONS 80

/* MemoryType values (Atmosphere/Horizon subset). */
#define HLEK_MEM_UNMAPPED   0x00u
#define HLEK_MEM_IO         0x01u
#define HLEK_MEM_NORMAL     0x02u
#define HLEK_MEM_CODE_STATIC 0x03u
#define HLEK_MEM_CODE_MUT  0x04u
#define HLEK_MEM_HEAP       0x05u
#define HLEK_MEM_STACK      0x0Au
#define HLEK_MEM_CODE_RO    0x13u
#define HLEK_MEM_CODE_WR    0x14u

/* Permission bits as Horizon reports them. */
#define HLEK_PERM_R 1u
#define HLEK_PERM_W 2u
#define HLEK_PERM_X 4u

/* InfoType values (svcGetInfo id0), the subset this layer answers. */
#define HLEK_INFO_ALIAS_REGION_ADDR   2u
#define HLEK_INFO_ALIAS_REGION_SIZE   3u
#define HLEK_INFO_HEAP_REGION_ADDR    4u
#define HLEK_INFO_HEAP_REGION_SIZE    5u
#define HLEK_INFO_TOTAL_MEMORY_SIZE   6u
#define HLEK_INFO_USED_MEMORY_SIZE    7u
#define HLEK_INFO_DEBUGGER_ATTACHED   8u
#define HLEK_INFO_IDLE_TICK_COUNT     10u
#define HLEK_INFO_RANDOM_ENTROPY      11u
#define HLEK_INFO_ASLR_REGION_ADDR    12u
#define HLEK_INFO_ASLR_REGION_SIZE    13u
#define HLEK_INFO_STACK_REGION_ADDR   14u
#define HLEK_INFO_STACK_REGION_SIZE   15u
#define HLEK_INFO_SYSRESOURCE_TOTAL   16u
#define HLEK_INFO_SYSRESOURCE_USED    17u
#define HLEK_INFO_INITIAL_PID_RANGE   19u
#define HLEK_INFO_IS_APPLICATION      23u
#define HLEK_INFO_FREE_THREAD_COUNT   24u

typedef struct HleKernelRegion {
    uint64_t base;
    uint64_t size;
    uint32_t type;
    uint32_t perm;
} HleKernelRegion;

/* A bounded trace of recent QueryMemory probes, kept so the caller can show
 * exactly how a guest (rtld's module scan) discovers the memory layout: each
 * probe's address and the region (base/size/type/perm) the kernel answered. */
#define HLEK_QUERY_TRACE 64

typedef struct HleKernelQuery {
    uint64_t addr;   /* address passed to QueryMemory          */
    uint64_t base;   /* region base it resolved to             */
    uint64_t size;   /* region size it resolved to             */
    uint32_t type;   /* MemoryType reported                    */
    uint32_t perm;   /* permission bits reported               */
} HleKernelQuery;

/* Optional sink for OutputDebugString (and other diagnostics). The string is
 * NUL-terminated; the platform decides where it goes. */
typedef void (*HleKernelLog)(void *ctx, const char *s);

/* A connected named-port session: the handle returned to the guest and the
 * port name it connected to (for honest diagnostics). A session may also have
 * been turned into a CMIF domain (the sm: service is one) and/or serve a
 * concrete service object (see ipc.c). */
#define HLEK_MAX_SESSIONS 32
#define HLEK_SESSION_NAME 24

enum {
    HLEK_OBJ_NONE = 0,   /* no dispatchable object (bare port)          */
    HLEK_OBJ_SM,         /* service manager ("sm:")                     */
    HLEK_OBJ_SERVICE     /* a concrete named service (fs, psm, ...)     */
};

typedef struct HleKernelSession {
    uint64_t handle;
    char     name[HLEK_SESSION_NAME];
    uint32_t object;      /* HLEK_OBJ_*                                 */
    int      is_domain;   /* converted to a CMIF domain                 */
    uint32_t domain_id;   /* this object's id within its domain (root)  */
    uint32_t next_obj_id; /* next id to hand out (root+1 after convert) */
    uint64_t domain_root; /* session owning the domain object table: its */
                          /* own handle, or the source of a CloneObject  */
} HleKernelSession;

/* An interface reached through a CMIF domain: id is only unique within the
 * owning session's domain. Dispatch resolves TargetObjectId back to this. */
#define HLEK_MAX_DOMAIN_OBJS 64

typedef struct HleKernelDomainObj {
    int      in_use;
    uint64_t session;     /* owning domain's root session handle         */
    uint32_t obj_id;      /* id within that domain                       */
    char     name[HLEK_SESSION_NAME];
} HleKernelDomainObj;

/* Transfer-memory kernel objects (svcCreateTransferMemory): a page-aligned
 * guest range handed to a service as a handle. The kernel's only role is to
 * validate the range and mint a handle; a service consuming it is modelled in
 * the IPC layer. Kept so CloseHandle and diagnostics see a real object. */
#define HLEK_MAX_TRANSFER_MEM 16

typedef struct HleKernelTransferMem {
    int      in_use;
    uint64_t handle;
    uint64_t addr;
    uint64_t size;
    uint32_t perm;
} HleKernelTransferMem;

/* Signallable kernel event objects (the waitable handles services hand back,
 * e.g. ICommonStateGetter::GetEventHandle). Handles are drawn from the same
 * allocator as sessions. Auto-clear events reset when waited on. */
#define HLEK_MAX_EVENTS 32

typedef struct HleKernelEvent {
    int      in_use;
    uint64_t handle;
    int      signaled;
    int      autoclear;
    char     name[HLEK_SESSION_NAME];
} HleKernelEvent;

/* nn::am::AppletMessage values (libnx applet.h / switchbrew). FocusStateChanged
 * is the one the real am service already has queued when an application first
 * enters its message loop, so it is what a booting game consumes first. */
enum {
    HLEK_APPLET_MSG_NONE                     = 0,
    HLEK_APPLET_MSG_EXIT_REQUEST             = 4,
    HLEK_APPLET_MSG_FOCUS_STATE_CHANGED      = 15,
    HLEK_APPLET_MSG_RESUME                   = 16,
    HLEK_APPLET_MSG_OPERATION_MODE_CHANGED   = 30,
    HLEK_APPLET_MSG_PERFORMANCE_MODE_CHANGED = 31,
    HLEK_APPLET_MSG_REQUEST_TO_DISPLAY       = 51
};

/* Optional heap placement hook. When installed, SetHeapSize asks the hook to
 * place a heap of `size` bytes and receives the guest base it was mapped at.
 * This lets a backend with a demand-paged heap window (see A64GuestMem_InitSparse)
 * answer a multi-gigabyte reservation it could never back in one flat buffer.
 * Return 0 to reject the size (SetHeapSize then declines the call). */
typedef int (*HleKernelHeapMap)(void *ctx, uint64_t size, uint64_t *out_base);

/* Bytes of the heap currently backed by real storage (UsedMemorySize), or 0
 * when the backend does not track it. */
typedef uint64_t (*HleKernelHeapUsed)(void *ctx);

typedef struct HleKernel {
    HleKernelRegion regions[HLEK_MAX_REGIONS];
    int      count;

    uint64_t span_end;   /* top of the address space tracked (exclusive)   */
    uint64_t heap_base;  /* set by SetHeapSize                             */
    uint64_t heap_size;

    /* Optional heap placement / accounting hooks (see above). */
    HleKernelHeapMap  heap_map;
    HleKernelHeapUsed heap_used;
    void             *heap_ctx;
    uint64_t tick;       /* monotonic counter for GetSystemTick            */
    int      exited;     /* ExitProcess was called                         */
    uint64_t process_id; /* reported by GetProcessId                       */
    uint64_t entropy[4]; /* GetInfo(RandomEntropy) pool (lazily filled)    */
    int      entropy_ready;

    /* Connected sessions and the handle allocator. */
    uint64_t        next_handle;
    HleKernelSession sessions[HLEK_MAX_SESSIONS];
    int             session_count;

    /* Interfaces handed out inside sessions converted to domains. */
    HleKernelDomainObj domain_objs[HLEK_MAX_DOMAIN_OBJS];

    /* Signallable event objects handed to the guest as handles. */
    HleKernelEvent events[HLEK_MAX_EVENTS];

    /* Transfer-memory objects (CreateTransferMemory). */
    HleKernelTransferMem tmem[HLEK_MAX_TRANSFER_MEM];

    /* nn::am applet message queue (see ICommonStateGetter::ReceiveMessage).
     * The event GetEventHandle hands out mirrors "a message is available": it
     * is signalled while the queue is non-empty and cleared when it drains,
     * exactly as the real am service manages its message event. */
#define HLEK_MAX_APPLET_MSGS 32
    uint32_t applet_msgs[HLEK_MAX_APPLET_MSGS];
    int      applet_head;    /* pop index                                  */
    int      applet_count;   /* messages pending                           */
    int      applet_evt;     /* index into events[] of the GetEventHandle
                              * event, or -1 before it has been handed out */

    HleKernelLog log;
    void        *log_ctx;

    /* Counters for honest reporting. */
    uint64_t query_count;
    uint64_t unknown_count;
    uint64_t last_unknown;

    /* Ring of the most recent QueryMemory probes (oldest at index 0 after the
     * count saturates). */
    HleKernelQuery qtrace[HLEK_QUERY_TRACE];
    int      qtrace_count;   /* entries currently held (<= HLEK_QUERY_TRACE) */
    int      qtrace_next;    /* next write position                        */
} HleKernel;

void HleKernel_Init(HleKernel *k);
void HleKernel_SetSpanEnd(HleKernel *k, uint64_t end);
void HleKernel_SetLog(HleKernel *k, HleKernelLog fn, void *ctx);

/* Install the heap placement/accounting hooks used by SetHeapSize and
 * GetInfo(UsedMemorySize). Pass NULL to fall back to the flat span_end model. */
void HleKernel_SetHeapMapper(HleKernel *k, HleKernelHeapMap map,
                             HleKernelHeapUsed used, void *ctx);
int  HleKernel_AddRegion(HleKernel *k, uint64_t base, uint64_t size,
                         uint32_t type, uint32_t perm);

/* Register this kernel's handlers into an HLE registry. */
void HleKernel_Register(HleState *h, HleKernel *k);

/* Port name a connected session handle refers to, or NULL if unknown. */
const char *HleKernel_SessionName(const HleKernel *k, uint64_t handle);

/* Find a session by handle, or NULL. */
HleKernelSession *HleKernel_FindSession(HleKernel *k, uint64_t handle);

/* Allocate a session (assigning the next handle), or NULL when full. */
HleKernelSession *HleKernel_NewSession(HleKernel *k, const char *name,
                                       uint32_t object);

/* Publish `name` as a new interface inside `s`'s domain, returning its object
 * id (>= 2), or 0 when the table is full. */
uint32_t HleKernel_NewDomainObj(HleKernel *k, HleKernelSession *s,
                                const char *name);

/* Interface name for `obj_id` inside `s`'s domain (the root object resolves
 * to the session's own name), or NULL when the id is unknown. */
const char *HleKernel_FindDomainObj(const HleKernel *k,
                                    const HleKernelSession *s, uint32_t obj_id);

/* Allocate a signallable event (assigning the next handle), or NULL when the
 * table is full. The event starts unsignalled. */
HleKernelEvent *HleKernel_NewEvent(HleKernel *k, const char *name,
                                   int autoclear);

/* Find an event by handle, or NULL. */
HleKernelEvent *HleKernel_FindEvent(HleKernel *k, uint64_t handle);

/* Find a transfer-memory object by handle, or NULL. */
HleKernelTransferMem *HleKernel_FindTransferMem(HleKernel *k, uint64_t handle);

/* Make `e` the applet message event returned by GetEventHandle and refresh its
 * signalled state from the current queue depth. Returns e. */
HleKernelEvent *HleKernel_BindAppletEvent(HleKernel *k, HleKernelEvent *e);

/* The bound applet message event, or NULL if GetEventHandle has not run. */
HleKernelEvent *HleKernel_AppletEvent(HleKernel *k);

/* Append an nn::am::AppletMessage; signals the bound event while non-empty. */
void HleKernel_PushAppletMessage(HleKernel *k, uint32_t msg);

/* Pop the oldest message. Returns 1 and fills *out, or 0 when the queue has
 * drained (which clears the bound event). */
int HleKernel_PopAppletMessage(HleKernel *k, uint32_t *out);

/* Human-readable name of a syscall number (for diagnostics; never NULL). */
const char *HleKernel_SvcName(uint64_t svc);

/* Number of QueryMemory probes currently held in the trace ring. */
int HleKernel_QueryTraceCount(const HleKernel *k);

/* Copy the i-th held probe (0 = oldest) into *out. Returns 1 on success. */
int HleKernel_QueryTraceAt(const HleKernel *k, int i, HleKernelQuery *out);

/* Describe how rtld's module scan treats the region a probe resolved to: the
 * exact accept/reject/stop reason (never NULL). */
const char *HleKernel_QueryVerdict(const HleKernelQuery *q);

#ifdef __cplusplus
}
#endif
