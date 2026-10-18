/* ipc.h: synchronous IPC transport + CMIF marshalling.
 *
 * Horizon user code talks to services through a session handle and a message
 * buffer: svcSendSyncRequest(handle) / svcSendSyncRequestWithUserBuffer(buf,
 * size, handle). The kernel copies the request out to the server and the
 * response back into the same buffer; the buffer is laid out as a HIPC header
 * followed by an optional CMIF message. This layer implements that transport
 * in-process (no separate server thread) so the SDK can drive real services.
 *
 * Nothing is faked: a request this layer does not implement returns 0 so the
 * caller reports the exact stop instead of inventing a response.
 */
#pragma once

#include <stdint.h>
#include "kernel.h"
#include "a64.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Handle a synchronous request sent to `session_handle`, using the guest
 * message buffer [buf, buf+size). Returns 1 if a response was written, 0 if
 * the message is not implemented (so the kernel reports the exact stop). */
int HleIpc_SendSyncRequest(HleKernel *k, A64 *cpu, uint64_t session_handle,
                           uint64_t buf, uint64_t size);

/* Short human-readable description of the last IPC request that was accepted
 * but not implemented (for diagnostics; never NULL). */
const char *HleIpc_LastUnhandled(void);

/* A bounded trace of recent IPC requests (for diagnostics). */
#define HLEIPC_TRACE 64
#define HLEIPC_NOTE  160

typedef struct HleIpcTrace {
    uint64_t handle;
    char     name[HLEK_SESSION_NAME];
    uint32_t type;      /* HIPC message type                              */
    uint32_t cmd;       /* CMIF method id (or manager cmd)                */
    int      is_sm;     /* target was the service manager                  */
    int      handled;   /* 1 = response written, 0 = unhandled             */
    char     note[HLEIPC_NOTE];
} HleIpcTrace;

int HleIpc_TraceCount(void);
int HleIpc_TraceAt(int i, HleIpcTrace *out);   /* i = 0 oldest */

#ifdef __cplusplus
}
#endif
