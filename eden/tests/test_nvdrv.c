/* Host-side test for the nvdrv (INvDrvServices) HIPC transport:
 * descriptor parsing, Open/Close file-descriptor bookkeeping, the
 * NVHOST_IOCTL_CTRL_GET_CONFIG decode + production-mode result, and the honest
 * stop at the first genuinely unsupported GPU ioctl.
 *
 * Build (from the repository root):
 *   gcc -std=c99 -Wall -Wextra -I emucore/a64 -I emucore/hle \
 *       eden/tests/test_nvdrv.c emucore/a64/a64.c emucore/a64/a64_mem.c \
 *       emucore/hle/hle.c emucore/hle/kernel.c emucore/hle/ipc.c \
 *       -o test_nvdrv && ./test_nvdrv
 */
#include <stdio.h>
#include <string.h>
#include "a64.h"
#include "a64_mem.h"
#include "kernel.h"
#include "ipc.h"
#include "nv_ioctl.h"

#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++fails; } } while (0)

#define GUEST_BASE 0x10000000u
#define MB         0x800u          /* message buffer offset in guest memory */
#define PATH_OFF   0x400u          /* where we stage device-path strings     */
#define ARG_OFF    0x600u          /* where we stage ioctl argument structs  */

#define CMIF_IN_MAGIC  0x49434653u
#define CMIF_OUT_MAGIC 0x4F434653u

static uint8_t g_buf[4096];

static char g_log[256];
static void my_log(void *ctx, const char *s) {
    (void)ctx;
    snprintf(g_log, sizeof(g_log), "%s", s);
}

static void wr32(A64GuestMem *m, uint32_t off, uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t)v;         b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
    A64GuestMem_Write(m, GUEST_BASE + off, b, 4);
}
static uint32_t rd32(A64GuestMem *m, uint32_t off) {
    uint8_t b[4];
    A64GuestMem_Read(m, GUEST_BASE + off, b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* Build a method request: header, optional send-static/send-buffer/recv-buffer
 * tables, then the CMIF in-header at the next 16-aligned raw offset. Returns
 * the CMIF parameter offset (buffer-relative). */
static uint32_t build_req_st(A64GuestMem *m, uint32_t nstat, uint32_t nsbuf,
                             uint32_t nrbuf, uint32_t cmd) {
    uint32_t st_off = 8;
    uint32_t sb_off = st_off + 8 * nstat;
    uint32_t rb_off = sb_off + 0xC * nsbuf;
    uint32_t dwo    = rb_off + 0xC * nrbuf;
    uint32_t raw    = (dwo + 15u) & ~15u;
    uint32_t params = raw + 0x10;
    uint32_t i;

    for (i = 0; i < 0x100; ++i) wr32(m, MB + 4 * i, 0);
    wr32(m, MB + 0, 4u | (nstat << 16) | (nsbuf << 20) | (nrbuf << 24));
    wr32(m, MB + 4, (params - dwo + 3u) / 4u);             /* num_data_words */
    wr32(m, MB + raw + 0x0, CMIF_IN_MAGIC);
    wr32(m, MB + raw + 0x4, 0);                            /* version */
    wr32(m, MB + raw + 0x8, cmd);
    wr32(m, MB + raw + 0xC, 0);                            /* token   */
    return params;
}
static uint32_t build_req(A64GuestMem *m, uint32_t nsbuf, uint32_t nrbuf,
                          uint32_t cmd) {
    return build_req_st(m, 0, nsbuf, nrbuf, cmd);
}

/* Write send-static descriptor `i` (table starts at MB+8). */
static void put_sstat(A64GuestMem *m, uint32_t i, uint64_t addr, uint32_t size) {
    uint32_t off = MB + 8 + 8 * i;
    wr32(m, off + 0, (size & 0xFFFFu) << 16);   /* index/addr high/mid = 0 */
    wr32(m, off + 4, (uint32_t)addr);
}

/* Write send-buffer descriptor `i` (tables start at MB+8). */
static void put_sbuf(A64GuestMem *m, uint32_t i, uint64_t addr, uint32_t size,
                     uint32_t mode) {
    uint32_t off = MB + 8 + 0xC * i;
    wr32(m, off + 0, size);                 /* size_low, size_high = 0        */
    wr32(m, off + 4, (uint32_t)addr);       /* address_low                    */
    /* word2 = mode | address_high<<2 | size_high<<26 | address_mid<<28 */
    wr32(m, off + 8, (mode & 3u) |
         (uint32_t)((addr >> 32) & 0xF) << 28);
}

/* Response accessors: with no special header the CMIF out-header lands at
 * buffer offset 0x10 and its parameters at 0x20. */
static uint32_t resp_magic(A64GuestMem *m) { return rd32(m, MB + 0x10); }
static uint32_t resp_param(A64GuestMem *m, uint32_t i) {
    return rd32(m, MB + 0x20 + 4 * i);
}

int main(void) {
    int fails = 0;
    A64GuestMem mem;
    A64 cpu;
    HleKernel k;
    HleKernelSession *sess;
    uint64_t h;
    uint32_t p, fd, err;

    A64GuestMem_Init(&mem, g_buf, sizeof(g_buf), GUEST_BASE);
    A64GuestMem_Attach(&mem, &cpu);
    HleKernel_Init(&k);
    HleKernel_SetSpanEnd(&k, 0x20000);
    HleKernel_SetLog(&k, my_log, NULL);

    sess = HleKernel_NewSession(&k, "nvdrv", HLEK_OBJ_SERVICE);
    CHECK(sess != NULL, "create nvdrv session");
    if (!sess) { printf("FAIL: no session\n"); return 1; }
    h = sess->handle;

    /* Initialize (cmd 3): Process + TransferMemory handles and a u32 size are
     * all ignored; Err=0. */
    p = build_req(&mem, 0, 0, 3);
    wr32(&mem, MB + p, 0x800000);
    CHECK(HleIpc_SendSyncRequest(&k, &cpu, h, GUEST_BASE + MB, 0x200) == 1,
          "Initialize handled");
    CHECK(resp_magic(&mem) == CMIF_OUT_MAGIC, "Initialize CMIF response");
    CHECK(resp_param(&mem, 0) == 0, "Initialize Err=0");

    /* SetAruid (cmd 8): Err=0. */
    p = build_req(&mem, 0, 0, 8);
    wr32(&mem, MB + p, 0x1000);
    CHECK(HleIpc_SendSyncRequest(&k, &cpu, h, GUEST_BASE + MB, 0x200) == 1,
          "SetAruid handled");
    CHECK(resp_param(&mem, 0) == 0, "SetAruid Err=0");

    /* Open (cmd 0) of a known device: the path comes from the type-0x5 send
     * buffer descriptor. Returns a live fd and Err=0. */
    {
        const char *path = "/dev/nvhost-ctrl";
        p = build_req(&mem, 1, 0, 0);
        A64GuestMem_Write(&mem, GUEST_BASE + PATH_OFF, (void *)path,
                          (uint32_t)strlen(path) + 1);
        put_sbuf(&mem, 0, GUEST_BASE + PATH_OFF, (uint32_t)strlen(path), 0);
        CHECK(HleIpc_SendSyncRequest(&k, &cpu, h, GUEST_BASE + MB, 0x200) == 1,
              "Open known device handled");
        fd = resp_param(&mem, 0);
        err = resp_param(&mem, 1);
        CHECK(fd != 0, "Open returned a non-zero fd");
        CHECK(err == 0, "Open Err=0");
        CHECK(strstr(g_log, "/dev/nvhost-ctrl") != NULL, "Open logged device");
    }

    /* Open an unknown device: FdOut=0, Err=FileOperationFailed (0x30003). */
    {
        const char *path = "/dev/definitely-not-a-device";
        build_req(&mem, 1, 0, 0);
        A64GuestMem_Write(&mem, GUEST_BASE + PATH_OFF, (void *)path,
                          (uint32_t)strlen(path) + 1);
        put_sbuf(&mem, 0, GUEST_BASE + PATH_OFF, (uint32_t)strlen(path), 0);
        CHECK(HleIpc_SendSyncRequest(&k, &cpu, h, GUEST_BASE + MB, 0x200) == 1,
              "Open unknown device handled");
        CHECK(resp_param(&mem, 0) == 0, "unknown device: no fd");
        CHECK(resp_param(&mem, 1) == 0x30003, "unknown device: Err");
    }

    /* Open with a descriptor pointing outside guest memory: the path read must
     * fail safely (BadParameter), never touch host memory. */
    build_req(&mem, 1, 0, 0);
    put_sbuf(&mem, 0, 0x20000000u, 32, 0);
    CHECK(HleIpc_SendSyncRequest(&k, &cpu, h, GUEST_BASE + MB, 0x200) == 1,
          "Open unreadable path handled");
    CHECK(resp_param(&mem, 1) == 0x4, "unreadable path: Err=BadParameter");

    /* Open with a declared send buffer but none staged (count lies): the decode
     * of descriptor 0 reads zeros -> size 0 -> unreadable path, no crash. */
    build_req(&mem, 0, 0, 0);
    CHECK(HleIpc_SendSyncRequest(&k, &cpu, h, GUEST_BASE + MB, 0x200) == 1,
          "Open with no send buffer handled");
    CHECK(resp_param(&mem, 1) == 0x4, "missing send buffer: Err=BadParameter");

    /* A descriptor whose table offset runs past the message must not be
     * trusted: declare 15 send buffers but stage a CMIF header where the table
     * would be; descriptor 0 reads from the CMIF bytes (garbage size/addr). */
    build_req(&mem, 15, 0, 0);
    CHECK(HleIpc_SendSyncRequest(&k, &cpu, h, GUEST_BASE + MB, 0x200) == 1,
          "Open with oversized buffer table handled");
    CHECK(resp_param(&mem, 1) == 0x4 || resp_param(&mem, 1) == 0x30003,
          "oversized table: descriptor not trusted");

    /* Close a live fd: Err=0. Closing it again: Err=BadParameter. */
    {
        const char *path = "/dev/nvmap";
        build_req(&mem, 1, 0, 0);
        A64GuestMem_Write(&mem, GUEST_BASE + PATH_OFF, (void *)path,
                          (uint32_t)strlen(path) + 1);
        put_sbuf(&mem, 0, GUEST_BASE + PATH_OFF, (uint32_t)strlen(path), 0);
        HleIpc_SendSyncRequest(&k, &cpu, h, GUEST_BASE + MB, 0x200);
        fd = resp_param(&mem, 0);
        CHECK(fd != 0, "open nvmap fd");

        p = build_req(&mem, 0, 0, 2);
        wr32(&mem, MB + p, fd);
        CHECK(HleIpc_SendSyncRequest(&k, &cpu, h, GUEST_BASE + MB, 0x200) == 1,
              "Close handled");
        CHECK(resp_param(&mem, 0) == 0, "Close Err=0");

        p = build_req(&mem, 0, 0, 2);
        wr32(&mem, MB + p, fd);
        HleIpc_SendSyncRequest(&k, &cpu, h, GUEST_BASE + MB, 0x200);
        CHECK(resp_param(&mem, 0) == 0x4, "double Close Err=BadParameter");

        p = build_req(&mem, 0, 0, 2);
        wr32(&mem, MB + p, 0xffffffu);
        HleIpc_SendSyncRequest(&k, &cpu, h, GUEST_BASE + MB, 0x200);
        CHECK(resp_param(&mem, 0) == 0x4, "Close unknown fd Err=BadParameter");
    }

    /* Ioctl request-number encoding: the macros must decode the known
     * NVHOST_IOCTL_CTRL_GET_CONFIG value (0xC183001B = inout, type 0x00,
     * nr 0x1B, size 0x183). */
    CHECK(NVHOST_IOCTL_CTRL_GET_CONFIG == 0xc183001bu, "GET_CONFIG encoding");
    CHECK(NV_IOC_DIR(0xc183001bu) == (NV_IOC_READ | NV_IOC_WRITE), "decode dir");
    CHECK(NV_IOC_TYPE(0xc183001bu) == NV_MAGIC_CUSTOM, "decode type");
    CHECK(NV_IOC_NR(0xc183001bu) == 0x1bu, "decode nr");
    CHECK(NV_IOC_SIZE(0xc183001bu) == 387, "decode size");
    CHECK(NVHOST_CTRL_CONFIG_SIZE == 387, "GET_CONFIG struct size");

    /* NVHOST_IOCTL_CTRL_GET_CONFIG on nvhost-ctrl (argument in a send static,
     * as the title's SDK sends it) is "not available in production mode", so
     * the verified result is Err=0x30006 -- the driver must not invent a
     * configuration value. */
    {
        const char *path = "/dev/nvhost-ctrl";
        uint8_t arg[NVHOST_CTRL_CONFIG_SIZE];
        build_req(&mem, 1, 0, 0);
        A64GuestMem_Write(&mem, GUEST_BASE + PATH_OFF, (void *)path,
                          (uint32_t)strlen(path) + 1);
        put_sbuf(&mem, 0, GUEST_BASE + PATH_OFF, (uint32_t)strlen(path), 0);
        HleIpc_SendSyncRequest(&k, &cpu, h, GUEST_BASE + MB, 0x200);
        fd = resp_param(&mem, 0);

        memset(arg, 0, sizeof(arg));
        memcpy(arg + NVHOST_CTRL_CONFIG_NAME_OFF, "nv", 3);
        memcpy(arg + NVHOST_CTRL_CONFIG_KEY_OFF, "NV_MEMORY_PROFILER", 19);
        A64GuestMem_Write(&mem, GUEST_BASE + ARG_OFF, arg, sizeof(arg));

        p = build_req_st(&mem, 1, 0, 0, 1);
        wr32(&mem, MB + p + 0, fd);
        wr32(&mem, MB + p + 4, NVHOST_IOCTL_CTRL_GET_CONFIG);
        put_sstat(&mem, 0, GUEST_BASE + ARG_OFF, NVHOST_CTRL_CONFIG_SIZE);
        CHECK(HleIpc_SendSyncRequest(&k, &cpu, h, GUEST_BASE + MB, 0x200) == 1,
              "GET_CONFIG handled");
        CHECK(resp_param(&mem, 0) == 0x30006,
              "GET_CONFIG Err=0x30006 (not available in production)");
        CHECK(strstr(g_log, "GET_CONFIG") != NULL, "GET_CONFIG logged");
        CHECK(strstr(g_log, "NV_MEMORY_PROFILER") != NULL,
              "GET_CONFIG key logged");
    }

    /* A GET_CONFIG whose argument is too short to hold name+key is refused
     * safely (no out-of-bounds read): it falls through to the honest stop. */
    {
        const char *path = "/dev/nvhost-ctrl";
        build_req(&mem, 1, 0, 0);
        A64GuestMem_Write(&mem, GUEST_BASE + PATH_OFF, (void *)path,
                          (uint32_t)strlen(path) + 1);
        put_sbuf(&mem, 0, GUEST_BASE + PATH_OFF, (uint32_t)strlen(path), 0);
        HleIpc_SendSyncRequest(&k, &cpu, h, GUEST_BASE + MB, 0x200);
        fd = resp_param(&mem, 0);

        p = build_req_st(&mem, 1, 0, 0, 1);
        wr32(&mem, MB + p + 0, fd);
        wr32(&mem, MB + p + 4, NVHOST_IOCTL_CTRL_GET_CONFIG);
        put_sstat(&mem, 0, GUEST_BASE + ARG_OFF, 8);   /* too small */
        CHECK(HleIpc_SendSyncRequest(&k, &cpu, h, GUEST_BASE + MB, 0x200) == 0,
              "short GET_CONFIG arg stops honestly");
    }

    /* Any other nvhost-ctrl ioctl must still stop honestly (return 0) and
     * report the decoded dir/type/nr/size. */
    {
        const char *path = "/dev/nvhost-ctrl";
        build_req(&mem, 1, 0, 0);
        A64GuestMem_Write(&mem, GUEST_BASE + PATH_OFF, (void *)path,
                          (uint32_t)strlen(path) + 1);
        put_sbuf(&mem, 0, GUEST_BASE + PATH_OFF, (uint32_t)strlen(path), 0);
        HleIpc_SendSyncRequest(&k, &cpu, h, GUEST_BASE + MB, 0x200);
        fd = resp_param(&mem, 0);

        p = build_req_st(&mem, 1, 0, 0, 1);
        wr32(&mem, MB + p + 0, fd);
        wr32(&mem, MB + p + 4, NVHOST_IOCTL_CTRL_SYNCPT_READ);
        put_sstat(&mem, 0, GUEST_BASE + ARG_OFF, 8);
        CHECK(HleIpc_SendSyncRequest(&k, &cpu, h, GUEST_BASE + MB, 0x200) == 0,
              "unimplemented ioctl stops honestly");
        CHECK(strstr(HleIpc_LastUnhandled(), "unimplemented") != NULL,
              "unimplemented ioctl reported");
        CHECK(strstr(HleIpc_LastUnhandled(), "nr=0x14") != NULL,
              "unimplemented ioctl decoded nr");
        CHECK(strstr(HleIpc_LastUnhandled(), "type=0x00") != NULL,
              "unimplemented ioctl decoded type");
    }

    /* A request to an unknown session handle is declined. */
    CHECK(HleIpc_SendSyncRequest(&k, &cpu, 0xdead, GUEST_BASE + MB, 0x200) == 0,
          "unknown session handle declined");
    CHECK(strstr(HleIpc_LastUnhandled(), "unknown handle") != NULL,
          "unknown handle reported");

    if (fails == 0) {
        printf("PASS: nvdrv (descriptors, Open/Close, GET_CONFIG decode, honest ioctl stop)\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", fails);
    return 1;
}
