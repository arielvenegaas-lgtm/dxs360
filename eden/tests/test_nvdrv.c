/* Host-side test for the nvdrv (INvDrvServices) HIPC transport:
 * descriptor parsing, Open/Close file-descriptor bookkeeping, and the honest
 * stop at the first unsupported GPU ioctl.
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

#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++fails; } } while (0)

#define GUEST_BASE 0x10000000u
#define MB         0x800u          /* message buffer offset in guest memory */
#define PATH_OFF   0x400u          /* where we stage device-path strings     */

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

    /* Ioctl (cmd 1) on an open nvhost-ctrl fd is NOT emulated: it must return
     * unhandled (0) and report the exact command so the caller stops cleanly. */
    {
        const char *path = "/dev/nvhost-ctrl";
        build_req(&mem, 1, 0, 0);
        A64GuestMem_Write(&mem, GUEST_BASE + PATH_OFF, (void *)path,
                          (uint32_t)strlen(path) + 1);
        put_sbuf(&mem, 0, GUEST_BASE + PATH_OFF, (uint32_t)strlen(path), 0);
        HleIpc_SendSyncRequest(&k, &cpu, h, GUEST_BASE + MB, 0x200);
        fd = resp_param(&mem, 0);

        /* The ioctl argp is carried in a send static (as the SDK does). */
        p = build_req_st(&mem, 1, 0, 0, 1);
        wr32(&mem, MB + p + 0, fd);
        wr32(&mem, MB + p + 4, 0xc183001bu);     /* iocode */
        put_sstat(&mem, 0, GUEST_BASE + PATH_OFF, 387);
        CHECK(HleIpc_SendSyncRequest(&k, &cpu, h, GUEST_BASE + MB, 0x200) == 0,
              "Ioctl is unhandled (honest stop)");
        CHECK(strstr(HleIpc_LastUnhandled(), "Ioctl") != NULL,
              "Ioctl reported");
        CHECK(strstr(HleIpc_LastUnhandled(), "0xc183001b") != NULL,
              "Ioctl iocode reported");
        CHECK(strstr(HleIpc_LastUnhandled(), "nvhost-ctrl") != NULL,
              "Ioctl device reported");
        CHECK(strstr(HleIpc_LastUnhandled(), "0x183") != NULL,
              "Ioctl input size reported");
    }

    /* A request to an unknown session handle is declined. */
    CHECK(HleIpc_SendSyncRequest(&k, &cpu, 0xdead, GUEST_BASE + MB, 0x200) == 0,
          "unknown session handle declined");
    CHECK(strstr(HleIpc_LastUnhandled(), "unknown handle") != NULL,
          "unknown handle reported");

    if (fails == 0) {
        printf("PASS: nvdrv (descriptors, Open/Close, errors, honest ioctl stop)\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", fails);
    return 1;
}
