/* ipc.c: synchronous IPC transport + CMIF marshalling.
 *
 * Layouts follow nn::sf::hipc::detail::HipcFormat and nn::sf::cmif (verified
 * against libnx's nx/include/switch/sf/{hipc.h,cmif.h}):
 *
 *   HIPC buffer
 *     0x0  u32 Header0: type:16 | send_statics:4 | send_buffers:4
 *                       | recv_buffers:4 | exch_buffers:4
 *     0x4  u32 Header1: data_words:10 | recv_static_mode:4 | pad:6
 *                       | recv_list_offset:11 | has_special:1
 *     0x8  u32 SpecialHeader (if has_special): pid:1 | copy:4 | move:4 | pad
 *     0xC  u64 ProcessId (if pid)
 *          u32 copy handles[], u32 move handles[]
 *          HipcStaticDescriptor send_statics[] (8 bytes each)
 *          HipcBufferDescriptor send/recv/exch[] (0xC bytes each)
 *          u32 data_words[]  <- raw data (CMIF)
 *          HipcRecvListEntry recv_list[]
 *
 *   CMIF raw data (at the u32 after 16-byte alignment from the buffer start)
 *          u32 CmifIn/OutHeader: magic | version | id/result | token (0x10)
 *          ...parameters...
 *
 * Nothing is faked: an unrecognised request returns 0 so the kernel reports
 * the exact stop instead of inventing a response.
 */
#include "ipc.h"
#include "nv_ioctl.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define HIPC_HDR        8u
#define CMIF_IN_MAGIC   0x49434653u  /* "SFCI" */
#define CMIF_OUT_MAGIC  0x4F434653u  /* "SFCO" */

/* Message type (HIPC Header0 bits 0-15). */
enum {
    MT_INVALID          = 0,
    MT_LEGACY_REQUEST   = 1,
    MT_CLOSE            = 2,
    MT_LEGACY_CONTROL   = 3,
    MT_REQUEST          = 4,   /* Invoke2MethodOld     */
    MT_CONTROL          = 5,   /* Invoke2ManagerMethodOld */
    MT_REQUEST_CONTEXT  = 6,   /* Invoke2Method        */
    MT_CONTROL_CONTEXT  = 7    /* Invoke2ManagerMethod */
};

/* CMIF manager (control) command ids. */
enum {
    CMIF_CMD_CONVERT_TO_DOMAIN   = 0,
    CMIF_CMD_COPY_FROM_DOMAIN    = 1,
    CMIF_CMD_CLONE_OBJECT        = 2,
    CMIF_CMD_QUERY_PTR_BUF_SIZE  = 3,
    CMIF_CMD_CLONE_OBJECT_EX     = 4
};

/* Object for a session created from an sm: GetServiceHandle name. */
static char g_last[192];   /* description of the last unhandled request */

/* Bounded trace of recent IPC requests. */
static HleIpcTrace g_trace[HLEIPC_TRACE];
static int g_trace_count;
static int g_trace_next;

int HleIpc_TraceCount(void) { return g_trace_count; }

int HleIpc_TraceAt(int i, HleIpcTrace *out) {
    int oldest, idx;
    if (i < 0 || i >= g_trace_count) return 0;
    oldest = (g_trace_count < HLEIPC_TRACE) ? 0 : g_trace_next;
    idx = (oldest + i) % HLEIPC_TRACE;
    *out = g_trace[idx];
    return 1;
}

static HleIpcTrace *trace_add(const HleKernelSession *s, uint32_t type,
                              uint32_t cmd, int is_sm) {
    HleIpcTrace *t = &g_trace[g_trace_next];
    memset(t, 0, sizeof(*t));
    t->handle = s ? s->handle : 0;
    if (s) {
        strncpy(t->name, s->name, HLEK_SESSION_NAME - 1);
    }
    t->type = type;
    t->cmd = cmd;
    t->is_sm = is_sm;
    g_trace_next = (g_trace_next + 1) % HLEIPC_TRACE;
    if (g_trace_count < HLEIPC_TRACE) ++g_trace_count;
    return t;
}

static int rd(A64 *c, uint64_t a, void *o, unsigned long n) {
    return c->read && c->read(c->memctx, a, o, n);
}
static int wr(A64 *c, uint64_t a, const void *o, unsigned long n) {
    return c->write && c->write(c->memctx, a, (void *)o, n);
}
static int rd32(A64 *c, uint64_t a, uint32_t *v) { return rd(c, a, v, 4); }

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ---- response builder -------------------------------------------------- */

typedef struct HipcOut {
    uint8_t  buf[0x400];
    uint32_t len;          /* bytes filled so far                            */
    uint32_t type;         /* echoes the request type (client ignores it)    */
    int      is_domain;    /* emit a CmifDomainOutHeader before the CMIF     */
    int      has_special;
    uint32_t num_copy, num_move;
    uint32_t copy_handles[8];
    uint32_t move_handles[8];
    uint32_t num_dobj;      /* domain object ids (CmifDomainOutHeader)       */
    uint32_t dobj[8];
    uint32_t data_off;     /* where data_words starts                        */
} HipcOut;

static void out_begin(HipcOut *o, uint32_t type) {
    memset(o, 0, sizeof(*o));
    o->type = type;
    o->len = HIPC_HDR;
}

static void out_add_move(HipcOut *o, uint32_t handle) {
    o->has_special = 1;
    o->move_handles[o->num_move++] = handle;
}

/* Hand a handle back as a *copy* (special-header copy array, bit1). Services
 * that return waitable objects (GetEventHandle, GetAcquiredSleepLockEvent, ...)
 * use SfOutHandleAttr_HipcCopy rather than a move. */
static void out_add_copy(HipcOut *o, uint32_t handle) {
    o->has_special = 1;
    o->copy_handles[o->num_copy++] = handle;
}

/* Hand out an interface as a domain object (a domain response must return
 * object ids in the CmifDomainOutHeader, never a move handle). */
static void out_add_dobj(HipcOut *o, uint32_t id) {
    if (o->num_dobj < 8) o->dobj[o->num_dobj++] = id;
}

/* Lay out the special header + handle arrays and return the data_words
 * offset. Must be called once, after handles are queued. */
static uint32_t out_layout(HipcOut *o) {
    uint32_t off = HIPC_HDR;
    if (o->has_special) {
        off += 4;
        o->len = off;
        if (o->num_copy) {
            memcpy(o->buf + off, o->copy_handles, 4 * o->num_copy);
            off += 4 * o->num_copy;
        }
        if (o->num_move) {
            memcpy(o->buf + off, o->move_handles, 4 * o->num_move);
            off += 4 * o->num_move;
        }
    }
    o->data_off = off;
    if (off > o->len) o->len = off;
    return off;
}

/* Write a CMIF out header (and leave room for params); returns the offset of
 * the first parameter word. A domain response carries a
 * CmifDomainOutHeader (0x10) before the CmifOutHeader. */
static uint32_t out_cmif(HipcOut *o, uint32_t version);
static uint32_t out_cmif_token(HipcOut *o, uint32_t version, uint32_t token);
static uint32_t out_cmif(HipcOut *o, uint32_t version) {
    return out_cmif_token(o, version, 0);
}

static uint32_t out_cmif_token(HipcOut *o, uint32_t version, uint32_t token) {
    uint32_t aligned, c;
    out_layout(o);                          /* place special header + handles */
    aligned = (o->data_off + 15u) & ~15u;
    c = aligned;
    if (o->is_domain) c += 0x10;
    memset(o->buf + o->data_off, 0, (c + 0x10) - o->data_off);
    put32(o->buf + c + 0x0, CMIF_OUT_MAGIC);
    put32(o->buf + c + 0x4, version);
    put32(o->buf + c + 0x8, 0);            /* Result: success */
    put32(o->buf + c + 0xC, token);        /* Token / InterfaceId */
    o->len = c + 0x10;
    return c + 0x10;
}

/* Lay down a CMIF out header carrying a non-zero Result (e.g. ReceiveMessage's
 * "no message available"). Returns the first-parameter offset.
 * The Result lives at (first-param off - 8) in both the domain and non-domain
 * layout: out_cmif_token returns header_off + 0x10 and the Result is the third
 * word of the 0x10-byte CmifOutHeader, so no extra domain term applies here
 * (the +0x10 for CmifDomainOutHeader is already inside `off`). */
static uint32_t out_cmif_result(HipcOut *o, uint32_t version, uint32_t result) {
    uint32_t off = out_cmif_token(o, version, 0);
    put32(o->buf + off - 0x08, result);
    return off;
}

static void out_param32(HipcOut *o, uint32_t off, uint32_t v) {
    put32(o->buf + off, v);
    if (off + 4 > o->len) o->len = off + 4;
}

/* CMIF raw output is laid out by natural alignment; `off` is the first raw
 * byte (16-aligned), so a leading u64 sits at offset 0. */
static void out_param64(HipcOut *o, uint32_t off, uint64_t v) {
    put32(o->buf + off,     (uint32_t)v);
    put32(o->buf + off + 4, (uint32_t)(v >> 32));
    if (off + 8 > o->len) o->len = off + 8;
}

/* Fill the HIPC header and flush the (word-rounded) message to the guest. */
static int out_finish(HipcOut *o, A64 *cpu, uint64_t buf) {
    uint32_t aligned = (o->data_off + 15u) & ~15u;
    uint32_t ndata, h0, h1, i;

    /* A domain response is laid out as:
     *   [aligned + 0x00] CmifDomainOutHeader { num_out_objects, pad[3] }
     *   [aligned + 0x10] CmifMessage { CmifOutHeader, params }   <- o->len
     *   [after it      ] DomainObjectId[num_out_objects]
     * (matches Atmosphere's DomainServiceObjectProcessor::PrepareForReply). */
    if (o->is_domain && o->num_dobj &&
        o->len + 4u * o->num_dobj <= sizeof(o->buf)) {
        put32(o->buf + aligned, o->num_dobj);
        for (i = 0; i < o->num_dobj; ++i)
            put32(o->buf + o->len + 4u * i, o->dobj[i]);
        o->len += 4u * o->num_dobj;
    }

    ndata = (aligned - o->data_off + (o->len - aligned) + 3u) / 4u;
    h0 = (o->type & 0xFFFFu);
    h1 = (ndata & 0x3FFu);
    if (o->len < aligned + 4) o->len = aligned + 4;  /* at least the header */
    if (o->has_special) {
        uint32_t sp = (o->num_move << 5) | (o->num_copy << 1);
        h1 |= (1u << 31);
        put32(o->buf + 0x8, sp);
    }
    put32(o->buf + 0x0, h0);
    put32(o->buf + 0x4, h1);
    o->len = (o->len + 7u) & ~7u;
    return wr(cpu, buf, o->buf, o->len);
}

/* ---- request parsing --------------------------------------------------- */

/* A decoded HipcStaticDescriptor (libnx sf/hipc.h):
 *   word0 index:6 | address_high:6 | address_mid:4 | size:16
 *   word1 address_low                                                     */
typedef struct HipcStatic {
    uint64_t addr;
    uint32_t size;
} HipcStatic;

/* A decoded HipcBufferDescriptor (libnx sf/hipc.h):
 *   word0 size_low
 *   word1 address_low
 *   word2 mode:2 | address_high:22 | size_high:4 | address_mid:4
 * address = address_low | address_mid<<32 | address_high<<36, size likewise. */
typedef struct HipcBuf {
    uint64_t addr;
    uint64_t size;
    uint32_t mode;
} HipcBuf;

typedef struct HipcIn {
    uint32_t type;
    int      has_special, send_pid;
    uint32_t num_copy, num_move;
    uint32_t num_send_statics, num_send_buffers, num_recv_buffers, num_exch;
    uint32_t send_statics_off;  /* buffer-relative, 0 if none            */
    uint32_t send_buffers_off;  /* buffer-relative, 0 if none            */
    uint32_t recv_buffers_off;  /* buffer-relative, 0 if none            */
    uint32_t data_words_off;    /* buffer-relative                       */
} HipcIn;

static int parse_hipc_in(A64 *cpu, uint64_t buf, HipcIn *in) {
    uint32_t h0, h1, off;
    uint32_t send_st, send_buf, recv_buf, exch;

    if (!rd32(cpu, buf + 0, &h0)) return 0;
    if (!rd32(cpu, buf + 4, &h1)) return 0;
    memset(in, 0, sizeof(*in));
    in->type      = h0 & 0xFFFFu;
    send_st       = (h0 >> 16) & 0xF;
    send_buf      = (h0 >> 20) & 0xF;
    recv_buf      = (h0 >> 24) & 0xF;
    exch          = (h0 >> 28) & 0xF;
    in->has_special = (int)((h1 >> 31) & 1);

    off = HIPC_HDR;
    if (in->has_special) {
        uint32_t sp;
        if (!rd32(cpu, buf + off, &sp)) return 0;
        off += 4;
        in->send_pid = (int)(sp & 1);
        in->num_copy = (sp >> 1) & 0xF;
        in->num_move = (sp >> 5) & 0xF;
        if (in->send_pid) off += 8;
        off += 4 * in->num_copy + 4 * in->num_move;
    }
    in->num_send_statics = send_st;
    in->num_send_buffers = send_buf;
    in->num_recv_buffers = recv_buf;
    in->num_exch         = exch;
    if (send_st) { in->send_statics_off = off; off += 8 * send_st; }
    if (send_buf) { in->send_buffers_off = off; off += 0xC * send_buf; }
    if (recv_buf) { in->recv_buffers_off = off; off += 0xC * recv_buf; }
    off += 0xC * exch;
    in->data_words_off = off;
    return 1;
}

/* Decode the send-static descriptor at `table_off` + 8*`i` (bounds-checked
 * against the table the header declared). */
static int hipc_send_static(A64 *cpu, uint64_t buf, const HipcIn *in,
                            uint32_t i, HipcStatic *out) {
    uint32_t w0, w1;
    if (!in->send_statics_off || i >= in->num_send_statics) return 0;
    if (!rd32(cpu, buf + in->send_statics_off + 8u * i + 0, &w0)) return 0;
    if (!rd32(cpu, buf + in->send_statics_off + 8u * i + 4, &w1)) return 0;
    out->size = (w0 >> 16) & 0xFFFFu;
    out->addr = w1 | ((uint64_t)((w0 >> 12) & 0xFu) << 32) |
                ((uint64_t)((w0 >> 6) & 0x3Fu) << 36);
    return 1;
}

/* Decode the buffer descriptor at `table_off` + 0xC*`i`. */
static int hipc_buf_at(A64 *cpu, uint64_t buf, uint32_t table_off,
                       uint32_t i, HipcBuf *out) {
    uint32_t w0, w1, w2;
    if (!table_off) return 0;
    if (!rd32(cpu, buf + table_off + 0xCu * i + 0, &w0)) return 0;
    if (!rd32(cpu, buf + table_off + 0xCu * i + 4, &w1)) return 0;
    if (!rd32(cpu, buf + table_off + 0xCu * i + 8, &w2)) return 0;
    out->mode = w2 & 0x3u;
    out->size = (uint64_t)w0 | ((uint64_t)((w2 >> 26) & 0xFu) << 32);
    out->addr = (uint64_t)w1 | ((uint64_t)((w2 >> 28) & 0xFu) << 32) |
                ((uint64_t)((w2 >> 2) & 0x3FFFFFu) << 36);
    return 1;
}

/* Decode send/recv-buffer descriptor `i` (bounds-checked against the table the
 * header declared). */
static int hipc_send_buffer(A64 *cpu, uint64_t buf, const HipcIn *in,
                            uint32_t i, HipcBuf *out) {
    if (i >= in->num_send_buffers) return 0;
    return hipc_buf_at(cpu, buf, in->send_buffers_off, i, out);
}

static int hipc_recv_buffer(A64 *cpu, uint64_t buf, const HipcIn *in,
                            uint32_t i, HipcBuf *out) {
    if (i >= in->num_recv_buffers) return 0;
    return hipc_buf_at(cpu, buf, in->recv_buffers_off, i, out);
}

/* Read a device path from a type-0x5 send buffer. The buffer holds exactly
 * strlen(path) bytes and is NOT required to be NUL-terminated (libnx's nvOpen
 * passes `strlen(devicepath)`), so read at most `size` bytes and always
 * terminate ourselves. Fails only on a guest-memory fault or an empty size. */
static int guest_path(A64 *cpu, uint64_t addr, uint64_t size,
                      char *out, size_t cap) {
    size_t n, i;
    if (cap == 0) return 0;
    if (size == 0) { out[0] = 0; return 0; }
    n = (size < cap - 1) ? (size_t)size : cap - 1;
    for (i = 0; i < n; ++i) {
        char ch;
        if (!rd(cpu, addr + i, &ch, 1)) { out[0] = 0; return 0; }
        out[i] = ch;
        if (ch == 0) return 1;
    }
    out[n] = 0;
    return 1;
}

/* Read the CMIF in-header at the request's aligned raw-data position.
 * Returns the buffer-relative offset of the CmifInHeader, or 0 on error. */
static uint32_t cmif_in_header(A64 *cpu, uint64_t buf, uint32_t data_words_off,
                               uint32_t *magic, uint32_t *version,
                               uint32_t *command_id, uint32_t *token) {
    uint32_t aligned = (data_words_off + 15u) & ~15u;
    if (!rd32(cpu, buf + aligned + 0x0, magic)) return 0;
    if (!rd32(cpu, buf + aligned + 0x4, version)) return 0;
    if (!rd32(cpu, buf + aligned + 0x8, command_id)) return 0;
    if (!rd32(cpu, buf + aligned + 0xC, token)) return 0;
    return aligned;
}

/* ---- sm: service manager ----------------------------------------------- */

static const char *type_name(uint32_t t) {
    switch (t) {
    case MT_LEGACY_REQUEST:  return "InvokeMethod";
    case MT_CLOSE:           return "Close";
    case MT_LEGACY_CONTROL:  return "InvokeManagerMethod";
    case MT_REQUEST:         return "Invoke2MethodOld";
    case MT_CONTROL:         return "Invoke2ManagerMethodOld";
    case MT_REQUEST_CONTEXT: return "Invoke2Method";
    case MT_CONTROL_CONTEXT: return "Invoke2ManagerMethod";
    default:                 return "Invalid";
    }
}
const char *HleIpc_LastUnhandled(void) { return g_last; }

/* Dispatch an sm: (service manager) command. Returns 1 if a response was
 * written (via *out), 0 if unimplemented. */
static int sm_dispatch(HleKernel *k, A64 *cpu, uint64_t buf, uint32_t params_off,
                       uint32_t cmd, HipcOut *out) {
    switch (cmd) {
    case 0: /* RegisterClient: input PID descriptor + reserved u64; no output */
        if (k->log) k->log(k->log_ctx, "sm: RegisterClient");
        out_cmif(out, 0);
        return 1;
    case 1: { /* GetServiceHandle: input u64 name; output moved Service handle */
        uint32_t lo = 0, hi = 0;
        uint64_t n;
        char name[HLEK_SESSION_NAME];
        HleKernelSession *s;
        int i;
        rd32(cpu, buf + params_off + 0, &lo);
        rd32(cpu, buf + params_off + 4, &hi);
        n = (uint64_t)lo | ((uint64_t)hi << 32);
        if (getenv("IPC_DBG")) {
            fprintf(stderr, "SVC_NAME raw lo=0x%08x hi=0x%08x n=0x%016llx "
                    "bytes[%02x %02x %02x %02x %02x %02x %02x %02x]\n",
                    lo, hi, (unsigned long long)n,
                    (unsigned)(n & 0xFF), (unsigned)((n >> 8) & 0xFF),
                    (unsigned)((n >> 16) & 0xFF), (unsigned)((n >> 24) & 0xFF),
                    (unsigned)((n >> 32) & 0xFF), (unsigned)((n >> 40) & 0xFF),
                    (unsigned)((n >> 48) & 0xFF), (unsigned)((n >> 56) & 0xFF));
        }
        for (i = 0; i < 8; ++i) {
            char ch = (char)((n >> (8 * i)) & 0xFF);
            if (ch == 0) break;
            name[i] = ch;
        }
        name[i] = 0;
        s = HleKernel_NewSession(k, name, HLEK_OBJ_SERVICE);
        if (s) {
            out_add_move(out, (uint32_t)s->handle);
            out_cmif(out, 0);
            if (k->log) {
                char line[HLEK_SESSION_NAME + 48];
                snprintf(line, sizeof(line),
                         "sm: GetServiceHandle(\"%s\") -> handle 0x%llx",
                         name, (unsigned long long)s->handle);
                k->log(k->log_ctx, line);
            }
            return 1;
        }
        out_cmif(out, 0);
        return 0;
    }
    case 2: /* RegisterService */
    case 3: /* UnregisterService */
    case 4: /* DetachClient */
    default:
        snprintf(g_last, sizeof(g_last), "sm: cmd %u unimplemented", cmd);
        return 0;
    }
}

/* ---- concrete services ------------------------------------------------- */

/* Publish an interface as a returned sub-object: a domain object id when the
 * session is a domain, otherwise a moved session handle. */
static int return_subobject(HleKernel *k, HleKernelSession *s,
                            const char *iface, HipcOut *out) {
    if (s->is_domain) {
        uint32_t id = HleKernel_NewDomainObj(k, s, iface);
        if (!id) return 0;
        out_add_dobj(out, id);
    } else {
        HleKernelSession *n = HleKernel_NewSession(k, iface, HLEK_OBJ_SERVICE);
        if (!n) return 0;
        out_add_move(out, (uint32_t)n->handle);
    }
    out_cmif(out, 0);
    return 1;
}

/* cmd -> sub-interface getter tables (verified against switchbrew). */
typedef struct IfaceGetter { uint32_t cmd; const char *iface; } IfaceGetter;

static int getter_dispatch(HleKernel *k, HleKernelSession *s,
                           const IfaceGetter *tbl, int n, uint32_t cmd,
                           HipcOut *out) {
    int i;
    for (i = 0; i < n; ++i)
        if (tbl[i].cmd == cmd)
            return return_subobject(k, s, tbl[i].iface, out);
    return 0;
}

/* nn::lm::ILogService ("lm"). On retail every command succeeds and does
 * nothing; the ILogService page documents Cmd 0 = OpenLogger(u64 pid) ->
 * ILogger instance. */
static int lm_dispatch(HleKernel *k, A64 *cpu, uint64_t buf, uint32_t p,
                       uint32_t cmd, HipcOut *out) {
    (void)cpu;
    switch (cmd) {
    case 0: { /* OpenLogger: input u64 pid; output ILogger handle */
        HleKernelSession *s = HleKernel_NewSession(k, "lm:logger", HLEK_OBJ_SERVICE);
        if (s) out_add_move(out, (uint32_t)s->handle);
        out_cmif(out, 0);
        (void)buf; (void)p;
        return 1;
    }
    default:
        return 0;
    }
}

/* nn::lm::ILogger. All commands succeed and do nothing on retail. */
static int lm_logger_dispatch(uint32_t cmd, HipcOut *out) {
    switch (cmd) {
    case 0:  /* Log (type-0x21 input buffer)     */
    case 1:  /* SetDestination (input u32 mask)  */
        out_cmif(out, 0);
        return 1;
    default:
        return 0;
    }
}

/* nn::nvdrv::INvDrvServices ("nvdrv"/"nvdrv:a"/"nvdrv:s"/"nvdrv:t").
 * switchbrew "NV services": Cmd 0=Open, 1=Ioctl, 2=Close, 3=Initialize,
 * 4=QueryEvent, 5=MapSharedMem, 6=GetStatus, 8=SetAruid, 11=Ioctl2, 12=Ioctl3.
 *
 * This layer implements only the transport-level bookkeeping that is honestly
 * satisfiable without a GPU: session Initialize, SetAruid, and the Open/Close
 * file-descriptor table for the device nodes nvn opens. The device nodes' ioctls
 * are NOT emulated -- Ioctl logs the exact unsupported command and returns
 * unhandled so the kernel reports the precise stop. No allocation, mapping or
 * rendering result is invented. */

/* NvResult codes (switchbrew "NV services" / nvtypes). */
enum {
    NV_SUCCESS                    = 0x0,
    NV_BAD_PARAMETER              = 0x4,
    NV_INSUFFICIENT_MEMORY        = 0x6,
    NV_FILE_OPERATION_FAILED      = 0x30003,
    /* NVHOST_IOCTL_CTRL_GET_CONFIG is "not available in production mode"
     * (switchbrew); yuzu/Ryujinx both return 0x30006 for it. */
    NV_NOT_AVAILABLE_IN_PRODUCTION = 0x30006
};

#define NVDRV_MAX_FD 16

typedef struct NvFdSlot {
    int      in_use;
    uint32_t fd;
    char     dev[32];
} NvFdSlot;

static NvFdSlot g_nvfd[NVDRV_MAX_FD];
static uint32_t g_nvfd_next = 1;

/* Device nodes nvn opens through nvdrv. Opening one only records an fd here;
 * its ioctls are handled (or refused) by the not-yet-implemented device layer. */
static const char *const k_nv_devices[] = {
    "/dev/nvmap",
    "/dev/nvhost-ctrl",
    "/dev/nvhost-ctrl-gpu",
    "/dev/nvhost-ctrl-nvdec",
    "/dev/nvhost-as-gpu",
    "/dev/nvhost-gpu",
    "/dev/nvhost-dbg-gpu",
    "/dev/nvhost-prof-gpu",
    "/dev/nvhost-vic",
    "/dev/nvhost-nvjpg",
    "/dev/nvhost-msenc",
    "/dev/nvhost-nvdec",
    "/dev/nvhost-tsec",
    "/dev/nvhost-isp",
    "/dev/nvhost-vi",
    "/dev/nvhost-emc",
    "/dev/nvdisp",
    "/dev/nvdisp-ctrl",
    "/dev/nvdisp-disp0"
};

static const char *nv_find_device(const char *path) {
    unsigned i;
    for (i = 0; i < sizeof(k_nv_devices) / sizeof(k_nv_devices[0]); ++i)
        if (strcmp(path, k_nv_devices[i]) == 0) return k_nv_devices[i];
    return NULL;
}

static const char *nv_fd_dev(uint32_t fd) {
    int i;
    for (i = 0; i < NVDRV_MAX_FD; ++i)
        if (g_nvfd[i].in_use && g_nvfd[i].fd == fd) return g_nvfd[i].dev;
    return NULL;
}

/* Locate the ioctl argument region. Transports differ: libnx sends a type-0x21
 * send buffer, while this title's SDK inlines the argument as a send static
 * (observed for NVHOST_IOCTL_CTRL_GET_CONFIG). Prefer the send static, then the
 * send buffer. Returns 0 if neither table is present. */
static int nv_ioctl_arg(A64 *cpu, uint64_t buf, const HipcIn *in,
                        uint64_t *addr, uint32_t *size) {
    HipcStatic ss;
    HipcBuf sb;
    if (hipc_send_static(cpu, buf, in, 0, &ss)) {
        *addr = ss.addr; *size = ss.size;
        return 1;
    }
    if (hipc_send_buffer(cpu, buf, in, 0, &sb)) {
        *addr = sb.addr; *size = sb.size;
        return 1;
    }
    return 0;
}

static int nvdrv_dispatch(HleKernel *k, A64 *cpu, uint64_t buf, const HipcIn *in,
                          uint32_t params_off, uint32_t cmd, HipcOut *out) {
    switch (cmd) {
    case 0: {  /* Open(type-0x5 buffer Path) -> u32 FdOut, u32 Err */
        HipcBuf pb;
        char path[64];
        const char *dev;
        uint32_t off;
        int slot = -1, i;

        if (!hipc_send_buffer(cpu, buf, in, 0, &pb) ||
            !guest_path(cpu, pb.addr, pb.size, path, sizeof(path))) {
            snprintf(g_last, sizeof(g_last), "nvdrv Open: unreadable path buffer");
            if (k->log) k->log(k->log_ctx, g_last);
            off = out_cmif(out, 0);
            out_param32(out, off + 0, 0);                 /* FdOut */
            out_param32(out, off + 4, NV_BAD_PARAMETER);  /* Err   */
            return 1;
        }
        dev = nv_find_device(path);
        if (!dev) {
            snprintf(g_last, sizeof(g_last),
                     "nvdrv Open(\"%s\"): unknown device -> Err=0x%x",
                     path, NV_FILE_OPERATION_FAILED);
            if (k->log) k->log(k->log_ctx, g_last);
            off = out_cmif(out, 0);
            out_param32(out, off + 0, 0);
            out_param32(out, off + 4, NV_FILE_OPERATION_FAILED);
            return 1;
        }
        for (i = 0; i < NVDRV_MAX_FD; ++i)
            if (!g_nvfd[i].in_use) { slot = i; break; }
        if (slot < 0) {
            off = out_cmif(out, 0);
            out_param32(out, off + 0, 0);
            out_param32(out, off + 4, NV_INSUFFICIENT_MEMORY);
            return 1;
        }
        g_nvfd[slot].in_use = 1;
        g_nvfd[slot].fd = g_nvfd_next++;
        strncpy(g_nvfd[slot].dev, dev, sizeof(g_nvfd[slot].dev) - 1);
        g_nvfd[slot].dev[sizeof(g_nvfd[slot].dev) - 1] = 0;
        if (k->log) {
            char line[80];
            snprintf(line, sizeof(line), "nvdrv Open(\"%s\") -> fd=%u",
                     dev, g_nvfd[slot].fd);
            k->log(k->log_ctx, line);
        }
        off = out_cmif(out, 0);
        out_param32(out, off + 0, g_nvfd[slot].fd);   /* FdOut */
        out_param32(out, off + 4, NV_SUCCESS);        /* Err   */
        return 1;
    }
    case 1: {  /* Ioctl(u32 fd, u32 iocode) + in/out buffers -> u32 Err */
        uint32_t fd = 0, iocode = 0;
        const char *dev;
        uint64_t aaddr = 0;
        uint32_t asize = 0;
        int have_arg;

        rd32(cpu, buf + params_off + 0, &fd);
        rd32(cpu, buf + params_off + 4, &iocode);
        dev = nv_fd_dev(fd);
        have_arg = nv_ioctl_arg(cpu, buf, in, &aaddr, &asize);

        /* NVHOST_IOCTL_CTRL_GET_CONFIG (name[0x41], key[0x41], value[0x101]):
         * verified against switchbrew "NV services". It is "not available in
         * production mode", so a retail title expects Err 0x30006 here. We
         * answer with that verified production result rather than inventing a
         * configuration value; the value[] region is left untouched. */
        if (dev && strcmp(dev, "/dev/nvhost-ctrl") == 0 &&
            iocode == NVHOST_IOCTL_CTRL_GET_CONFIG &&
            have_arg && asize >= NVHOST_CTRL_CONFIG_SIZE) {
            char name[NVHOST_CTRL_CONFIG_NAME_SIZE];
            char key[NVHOST_CTRL_CONFIG_KEY_SIZE];
            char line[192];
            uint32_t off;
            if (!rd(cpu, aaddr + NVHOST_CTRL_CONFIG_NAME_OFF, name, sizeof(name) - 1))
                name[0] = 0;
            name[sizeof(name) - 1] = 0;
            if (!rd(cpu, aaddr + NVHOST_CTRL_CONFIG_KEY_OFF, key, sizeof(key) - 1))
                key[0] = 0;
            key[sizeof(key) - 1] = 0;
            snprintf(line, sizeof(line),
                     "nvdrv GET_CONFIG \"%s!%s\" -> Err=0x%x (production)",
                     name, key, NV_NOT_AVAILABLE_IN_PRODUCTION);
            if (k->log) k->log(k->log_ctx, line);
            off = out_cmif(out, 0);
            out_param32(out, off, NV_NOT_AVAILABLE_IN_PRODUCTION);
            return 1;
        }

        /* Any other ioctl stays unhandled: report the exact decoded fields. */
        {
            HipcBuf rb;
            int hr = hipc_recv_buffer(cpu, buf, in, 0, &rb);
            snprintf(g_last, sizeof(g_last),
                     "nvdrv Ioctl fd=%u dev=\"%s\" iocode=0x%08x "
                     "dir=%u type=0x%02x nr=0x%02x size=0x%x "
                     "arg{0x%llx,0x%x} rbuf{0x%llx,0x%llx}: GPU ioctl unimplemented",
                     fd, dev ? dev : "?", iocode,
                     NV_IOC_DIR(iocode), NV_IOC_TYPE(iocode), NV_IOC_NR(iocode),
                     NV_IOC_SIZE(iocode),
                     (unsigned long long)(have_arg ? aaddr : 0),
                     have_arg ? asize : 0,
                     (unsigned long long)(hr ? rb.addr : 0),
                     (unsigned long long)(hr ? rb.size : 0));
            if (k->log) k->log(k->log_ctx, g_last);
            return 0;   /* honest stop at the first unimplemented GPU ioctl */
        }
    }
    case 2: {  /* Close(u32 fd) -> u32 Err */
        uint32_t fd = 0, off;
        int i, found = 0;
        rd32(cpu, buf + params_off + 0, &fd);
        for (i = 0; i < NVDRV_MAX_FD; ++i)
            if (g_nvfd[i].in_use && g_nvfd[i].fd == fd) {
                g_nvfd[i].in_use = 0;
                found = 1;
                break;
            }
        off = out_cmif(out, 0);
        out_param32(out, off, found ? NV_SUCCESS : NV_BAD_PARAMETER);
        return 1;
    }
    case 3:    /* Initialize(Process, TransferMemory, u32 size) -> u32 Err */
    case 8: {  /* SetAruid(u64 AppletResourceUserId, PID-desc) -> u32 Err */
        uint32_t off = out_cmif(out, 0);
        out_param32(out, off, NV_SUCCESS);
        return 1;
    }
    default:
        return 0;
    }
}

/* nn::am::service::IApplicationProxyService ("appletOE"). */
static const IfaceGetter k_applet_oe_getters[] = {
    { 0, "IApplicationProxy" },
};

/* nn::am::service::IApplicationProxy: every command returns a sub-interface.
 * (switchbrew "Applet Manager services") */
static const IfaceGetter k_app_proxy_getters[] = {
    {    0, "ICommonStateGetter" },
    {    1, "ISelfController" },
    {    2, "IWindowController" },
    {    3, "IAudioController" },
    {    4, "IDisplayController" },
    {   10, "IProcessWindingController" },
    {   11, "ILibraryAppletCreator" },
    {   20, "IApplicationFunctions" },
    {   40, "IPerformanceFunctions" },
    {   60, "IGraphicsScalingParametersFunctions" },
    { 1000, "IDebugFunctions" },
};

static int appletoe_dispatch(HleKernel *k, HleKernelSession *s, uint32_t cmd,
                             HipcOut *out) {
    return getter_dispatch(k, s, k_applet_oe_getters,
                           (int)(sizeof(k_applet_oe_getters) /
                                 sizeof(k_applet_oe_getters[0])),
                           cmd, out);
}

static int app_proxy_dispatch(HleKernel *k, HleKernelSession *s, uint32_t cmd,
                              HipcOut *out) {
    return getter_dispatch(k, s, k_app_proxy_getters,
                           (int)(sizeof(k_app_proxy_getters) /
                                 sizeof(k_app_proxy_getters[0])),
                           cmd, out);
}

/* nn::am::service::IWindowController (switchbrew). Commands with a documented
 * shape are handled; CreateWindow (0) is stubbed to an error on retail and is
 * deliberately left to surface as an exact stop if this title ever issues it. */
static int window_controller_dispatch(HleKernel *k, HleKernelSession *s,
                                      uint32_t cmd, HipcOut *out) {
    (void)s;
    switch (cmd) {
    case 1: /* GetAppletResourceUserId               -> u64 */
    case 2: /* GetAppletResourceUserIdOfCallerApplet -> u64 [6.0.0+] */
        out_param64(out, out_cmif(out, 0), k->process_id);
        return 1;
    case 10: /* AcquireForegroundRights       (no in/out) */
    case 11: /* ReleaseForegroundRights       (no in/out) */
    case 12: /* RejectToChangeIntoBackground  (no in/out) */
    case 20: /* SetAppletWindowVisibility     (u8 in)     */
    case 21: /* SetAppletGpuTimeSlice         (s64 in)    */
        out_cmif(out, 0);
        return 1;
    default:
        return 0;
    }
}

/* nn::am::service::ISelfController (switchbrew). The SDK drives these while
 * handling the boot FocusStateChanged message (SetFocusHandlingMode first).
 * Every command answered here is documented as taking input and returning no
 * output, so "accept and do nothing" is its correct observable behaviour.
 * Side-effecting commands (0 Exit, 9 GetLibraryAppletLaunchableEvent, 40
 * CreateManagedDisplayLayer, ...) are deliberately left to surface as exact
 * stops until the title actually needs them. */
static int self_controller_dispatch(HleKernel *k, HleKernelSession *s,
                                    uint32_t cmd, HipcOut *out) {
    (void)k; (void)s;
    switch (cmd) {
    case 1:  /* LockExit                                    (no in/out) */
    case 2:  /* UnlockExit                                  (no in/out) */
    case 3:  /* EnterFatalSection                           (no in/out) */
    case 4:  /* LeaveFatalSection                           (no in/out) */
    case 10: /* SetScreenShotPermission                     (s32 in)   */
    case 11: /* SetOperationModeChangedNotification         (u8 in)    */
    case 12: /* SetPerformanceModeChangedNotification       (u8 in)    */
    case 13: /* SetFocusHandlingMode                        (3x u8 in) */
    case 14: /* SetRestartMessageEnabled                    (u8 in)    */
    case 16: /* SetOutOfFocusSuspendingEnabled              (u8 in)    */
    case 17: /* SetControllerFirmwareUpdateSection          (u8 in)    */
    case 18: /* SetRequiresCaptureButtonShortPressedMessage (u8 in)    */
    case 19: /* SetAlbumImageOrientation                    (u8 in)    */
    case 20: /* SetDesirableKeyboardLayout                  (u32 in)   */
    case 50: /* SetHandlesRequestToDisplay                  (u8 in)    */
    case 51: /* ApproveToDisplay                            (no in/out) */
    case 60: /* OverrideAutoSleepTimeAndDimmingTime         (4x s32 in)*/
    case 61: /* SetMediaPlaybackState                       (u32 in)   */
    case 62: /* SetIdleTimeDetectionExtension               (u32 in)   */
    case 65: /* ReportUserIsActive                          (no in/out) */
        out_cmif(out, 0);
        return 1;
    default:
        return 0;
    }
}

/* nn::am::service::ICommonStateGetter (switchbrew). Only commands with a
 * documented shape are answered. GetEventHandle returns the am message event
 * (signalled while a message is pending) and ReceiveMessage pops the queued
 * nn::am::AppletMessage, so the SDK's applet-init message loop consumes the
 * boot focus transition exactly as it does on hardware. */
static int common_state_getter_dispatch(HleKernel *k, HleKernelSession *s,
                                        uint32_t cmd, HipcOut *out) {
    (void)s;
    switch (cmd) {
    case 0: { /* GetEventHandle: no input -> out Copy handle to a waitable event */
        /* autoclear=0: the service owns the signal state, keeping it set while
         * a message is pending and clearing it when the queue drains. */
        HleKernelEvent *e = HleKernel_NewEvent(k, "ICommonStateGetter:Event", 0);
        if (!e) return 0;
        HleKernel_BindAppletEvent(k, e);
        out_add_copy(out, (uint32_t)e->handle);
        out_cmif(out, 0);
        return 1;
    }
    case 1: { /* ReceiveMessage: no input -> out u32 AppletMessage.
               * 0x680 = no message available (switchbrew). */
        uint32_t msg = 0;
        if (HleKernel_PopAppletMessage(k, &msg))
            out_param32(out, out_cmif(out, 0), msg);
        else
            out_cmif_result(out, 0, 0x680u);
        return 1;
    }
    case 5: /* GetOperationMode -> u8 (0 = Handheld)      */
    case 6: /* GetPerformanceMode -> u32 (0 = Normal)     */
    case 7: /* GetCradleStatus -> u8                      */
    case 8: /* GetBootMode -> u8                          */
        out_param32(out, out_cmif(out, 0), 0);
        return 1;
    case 9: /* GetCurrentFocusState -> u8 (1 = InFocus)   */
        out_param32(out, out_cmif(out, 0), 1);
        return 1;
    case 10: /* RequestToAcquireSleepLock      (no in/out) */
    case 11: /* ReleaseSleepLock                (no in/out) */
    case 12: /* ReleaseSleepLockTransiently     (no in/out) */
        out_cmif(out, 0);
        return 1;
    default:
        return 0;
    }
}

/* nn::pctl::detail::ipc::IParentalControlServiceFactory ("pctl", "pctl:a",
 * "pctl:s", "pctl:r"). Cmd 0 CreateService and Cmd 1
 * CreateServiceWithoutInitialize (4.0.0+) take only the client pid (delivered
 * via the HIPC special header) and return an nn::pctl::detail::ipc::
 * IParentalControlService -- a new interface object in the caller's domain
 * (matches Ryujinx's MakeObject). Its own dispatcher answers the queries. */
static int pctl_factory_dispatch(HleKernel *k, HleKernelSession *s,
                                 uint32_t cmd, HipcOut *out) {
    switch (cmd) {
    case 0:  /* CreateService                  -> IParentalControlService */
    case 1:  /* CreateServiceWithoutInitialize  (4.0.0+)                 */
        return return_subobject(k, s, "IParentalControlService", out);
    default:
        return 0;
    }
}

/* nn::am::service::IApplicationFunctions (exposed on IApplicationProxy).
 * Startup queries a title makes through the application proxy. Commands whose
 * result is empty or a fixed value are answered; data-bearing ones
 * (GetDesiredLanguage, GetDisplayVersion, GetPseudoDeviceId, EnsureSaveData,
 * ...) are left to surface as exact stops until their backing data exists. */
static int app_funcs_dispatch(HleKernel *k, HleKernelSession *s,
                              uint32_t cmd, HipcOut *out) {
    (void)k; (void)s;
    switch (cmd) {
    case 22: /* SetTerminateResult: u32 in, no output               */
    case 30: /* BeginBlockingHomeButtonShortAndLongPressed (s64 in) */
    case 31: /* EndBlockingHomeButtonShortAndLongPressed            */
    case 32: /* BeginBlockingHomeButton                    (s64 in) */
    case 33: /* EndBlockingHomeButton                               */
    case 60: /* SetMediaPlaybackStateForApplication        (u8 in)  */
    case 67: /* SetGamePlayRecordingState                  (u32 in) */
        out_cmif(out, 0);
        return 1;
    case 40: /* NotifyRunning -> u8 bool (ignored by callers)       */
        out_param32(out, out_cmif(out, 0), 1);
        return 1;
    case 65: /* IsGamePlayRecordingSupported -> u8 bool (unsupported) */
        out_param32(out, out_cmif(out, 0), 0);
        return 1;
    default:
        return 0;
    }
}

/* Dispatch a method message to a service session by name. */
static int service_dispatch(HleKernel *k, A64 *cpu, uint64_t buf,
                            const HipcIn *in, uint32_t params_off, uint32_t cmd,
                            const char *name, HleKernelSession *s,
                            HipcOut *out) {
    if (strcmp(name, "nvdrv") == 0 || strcmp(name, "nvdrv:a") == 0 ||
        strcmp(name, "nvdrv:s") == 0 || strcmp(name, "nvdrv:t") == 0)
        return nvdrv_dispatch(k, cpu, buf, in, params_off, cmd, out);
    if (strcmp(name, "lm") == 0)
        return lm_dispatch(k, cpu, buf, params_off, cmd, out);
    if (strcmp(name, "lm:logger") == 0)
        return lm_logger_dispatch(cmd, out);
    if (strcmp(name, "appletOE") == 0)
        return appletoe_dispatch(k, s, cmd, out);
    if (strcmp(name, "IApplicationProxy") == 0)
        return app_proxy_dispatch(k, s, cmd, out);
    if (strcmp(name, "IWindowController") == 0)
        return window_controller_dispatch(k, s, cmd, out);
    if (strcmp(name, "ISelfController") == 0)
        return self_controller_dispatch(k, s, cmd, out);
    if (strcmp(name, "ICommonStateGetter") == 0)
        return common_state_getter_dispatch(k, s, cmd, out);
    if (strcmp(name, "IApplicationFunctions") == 0)
        return app_funcs_dispatch(k, s, cmd, out);
    if (strcmp(name, "pctl") == 0 || strcmp(name, "pctl:a") == 0 ||
        strcmp(name, "pctl:s") == 0 || strcmp(name, "pctl:r") == 0)
        return pctl_factory_dispatch(k, s, cmd, out);
    return 0;
}

/* ---- entry point ------------------------------------------------------- */

int HleIpc_SendSyncRequest(HleKernel *k, A64 *cpu, uint64_t handle,
                           uint64_t buf, uint64_t size) {
    HipcIn in;
    uint32_t magic = 0, version = 0, cmd = 0, req_token = 0;
    uint32_t raw, params;
    uint32_t domain_type = 0, domain_objects = 0, domain_target = 0;
    uint32_t domain_inraw = 0;
    int is_control, is_method, is_close, handled = 0;
    HleKernelSession *s;
    HipcOut out;
    HleIpcTrace *tr;
    (void)size;

    g_last[0] = 0;
    s = HleKernel_FindSession(k, handle);
    if (!s) {
        snprintf(g_last, sizeof(g_last), "send to unknown handle 0x%llx",
                 (unsigned long long)handle);
        return 0;
    }
    if (!parse_hipc_in(cpu, buf, &in)) return 0;

    if (getenv("IPC_DBG")) {
        uint32_t h0, h1, mg;
        rd32(cpu, buf + 0, &h0);
        rd32(cpu, buf + 4, &h1);
        rd32(cpu, buf + ((in.data_words_off + 15u) & ~15u), &mg);
        fprintf(stderr,
                "IPC_DBG handle=0x%llx buf=0x%llx h0=0x%08x h1=0x%08x "
                "type=%u sp=%d copy=%u move=%u dwo=0x%x aligned=0x%x magic=0x%08x\n",
                (unsigned long long)handle, (unsigned long long)buf, h0, h1,
                in.type, in.has_special, in.num_copy, in.num_move,
                in.data_words_off, (in.data_words_off + 15u) & ~15u, mg);
    }

    is_control = (in.type == MT_CONTROL || in.type == MT_LEGACY_CONTROL ||
                  in.type == MT_CONTROL_CONTEXT);
    is_method  = (in.type == MT_LEGACY_REQUEST || in.type == MT_REQUEST ||
                  in.type == MT_REQUEST_CONTEXT);
    is_close   = (in.type == MT_CLOSE);

    if (getenv("IPC_DBG2")) {
        fprintf(stderr, "IPC_DBG2 type=%u ctrl=%d meth=%d close=%d\n",
                in.type, is_control, is_method, is_close);
    }

    raw = (in.data_words_off + 15u) & ~15u;

    if (is_method && s->is_domain) {
        /* A CmifDomainInHeader precedes the CmifInHeader:
         *   0x0 u8 RequestKind  0x1 u8 InObjectCount  0x2 u16 InRawSize
         *   0x4 u32 TargetObjectId 0x8 u32 Reserved  0xC u32 Token */
        uint32_t d0;
        if (!rd32(cpu, buf + raw + 0x0, &d0)) return 0;
        domain_type    = d0 & 0xFFu;
        domain_objects = (d0 >> 8) & 0xFFu;
        domain_inraw   = (d0 >> 16) & 0xFFFFu;
        (void)rd32(cpu, buf + raw + 0x4, &domain_target);
        if (getenv("IPC_DBG")) {
            fprintf(stderr, "IPC_DOM kind=%u inobj=%u inraw=0x%x target=%u\n",
                    domain_type, domain_objects, domain_inraw, domain_target);
        }
        if (!cmif_in_header(cpu, buf, raw + 0x10, &magic, &version, &cmd,
                            &req_token) || magic != CMIF_IN_MAGIC) {
            snprintf(g_last, sizeof(g_last), "bad domain cmif on \"%s\"", s->name);
            return 0;
        }
        params = raw + 0x20;
    } else if (is_close) {
        /* Close (type 2) carries no CMIF payload -- the client reuses the
         * buffer, so any CMIF magic present is a stale leftover. */
        params = raw;
    } else {
        if (!cmif_in_header(cpu, buf, in.data_words_off, &magic, &version, &cmd,
                            &req_token) || magic != CMIF_IN_MAGIC) {
            snprintf(g_last, sizeof(g_last), "bad cmif magic 0x%08x on \"%s\"",
                     magic, s->name);
            return 0;
        }
        params = raw + 0x10;
    }

    tr = trace_add(s, in.type, cmd, s->object == HLEK_OBJ_SM);

    out_begin(&out, in.type);

    if (is_control) {
        switch (cmd) {
        case CMIF_CMD_CONVERT_TO_DOMAIN:
            s->is_domain = 1;
            s->domain_id = 1;
            s->next_obj_id = 2;
            s->domain_root = s->handle;
            out_param32(&out, out_cmif_token(&out, version, req_token), 1);
            handled = 1;
            break;
        case CMIF_CMD_QUERY_PTR_BUF_SIZE:
            out_cmif_token(&out, version, req_token);   /* no pointer buffer -> 0 */
            handled = 1;
            break;
        case CMIF_CMD_CLONE_OBJECT:
        case CMIF_CMD_CLONE_OBJECT_EX:
        case CMIF_CMD_COPY_FROM_DOMAIN: {
            HleKernelSession *c = HleKernel_NewSession(k, s->name, s->object);
            if (c) {
                /* A clone of a domain session joins the same domain: it must
                 * keep the domain flag and resolve the owner's object table. */
                if (s->is_domain) {
                    c->is_domain = 1;
                    c->domain_id = s->domain_id;
                    c->next_obj_id = s->next_obj_id;
                    c->domain_root = s->domain_root ? s->domain_root : s->handle;
                }
                out_add_move(&out, (uint32_t)c->handle);
            }
            out_cmif_token(&out, version, req_token);
            handled = 1;
            break;
        }
        default:
            snprintf(g_last, sizeof(g_last), "cmif control cmd %u unimplemented", cmd);
            break;
        }
    } else if (is_close) {
        handled = 1;
    } else if (is_method) {
        const char *iface = s->name;
        out.is_domain = s->is_domain;
        /* Inside a domain every message targets one object; dispatch to the
         * interface that object id was published as. */
        if (s->is_domain) {
            const char *dn = HleKernel_FindDomainObj(k, s, domain_target);
            if (!dn) {
                snprintf(g_last, sizeof(g_last),
                         "unknown domain object %u on \"%s\"", domain_target,
                         s->name);
                if (k->log) k->log(k->log_ctx, g_last);
                return 0;
            }
            iface = dn;
        }
        (void)domain_type; (void)domain_objects;
        if (s->object == HLEK_OBJ_SM) {
            handled = sm_dispatch(k, cpu, buf, params, cmd, &out);
            if (!handled)
                snprintf(g_last, sizeof(g_last), "sm: %s cmd %u unimplemented",
                         type_name(in.type), cmd);
        } else {
            handled = service_dispatch(k, cpu, buf, &in, params, cmd, iface,
                                       s, &out);
            if (!handled && !g_last[0])
                snprintf(g_last, sizeof(g_last),
                         "service \"%s\" %s cmd %u unimplemented",
                         iface, type_name(in.type), cmd);
        }
    } else {
        snprintf(g_last, sizeof(g_last), "message type %u unimplemented", in.type);
    }

    if (!handled) {
        if (tr) {
            strncpy(tr->note, g_last, HLEIPC_NOTE - 1);
            tr->note[HLEIPC_NOTE - 1] = 0;
        }
        if (k->log) k->log(k->log_ctx, g_last);
        return 0;
    }
    /* Rewrite cmif token to match request token for method/control */
    if (is_method || is_control) {
        uint32_t raw = (in.data_words_off + 15u) & ~15u;
        uint32_t aligned = (raw + 15u) & ~15u;
        uint32_t cmif = aligned;
        if (out.is_domain) cmif += 0x10;
        put32(out.buf + cmif + 0xC, req_token);
    }
    if (!out_finish(&out, cpu, buf)) return 0;
    if (tr) {
        tr->handled = handled;
        strncpy(tr->note, g_last, HLEIPC_NOTE - 1);
    }
    return 1;
}

