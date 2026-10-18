/* Host-side test for the nn::visrv root-service HLE ("vi:m" =
 * nn::visrv::sf::IManagerRootService, switchbrew "Display services").
 *
 * vi:m cmd 2 GetDisplayService(u32 serviceType) returns an
 * nn::visrv::sf::IApplicationDisplayService and nothing else. This test pins
 * that down: the returned handle is a live session named for that interface,
 * its documented get-session commands (100/101/102) hand out their sub-services
 * by name, and every data-bearing command (ListDisplays/OpenDisplay/...) still
 * stops honestly, so no connected display is ever fabricated.
 *
 * Build (from the repository root):
 *   gcc -std=c99 -Wall -Wextra -I emucore/a64 -I emucore/hle \
 *       eden/tests/test_vi.c emucore/a64/a64.c emucore/a64/a64_mem.c \
 *       emucore/hle/hle.c emucore/hle/kernel.c emucore/hle/ipc.c \
 *       -o test_vi && ./test_vi
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

#define CMIF_IN_MAGIC  0x49434653u /* "SFCI" */
#define CMIF_OUT_MAGIC 0x4F434653u /* "SFCO" */

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

/* Build a plain method request (no descriptors): HIPC header, then the CMIF
 * in-header at the 16-aligned raw offset, then params. Returns the
 * buffer-relative parameter offset. */
static uint32_t build_method(A64GuestMem *m, uint32_t cmd) {
    uint32_t i;
    for (i = 0; i < 0x100; ++i) wr32(m, MB + 4 * i, 0);
    wr32(m, MB + 0x0, 4u);            /* type = Invoke2MethodOld          */
    wr32(m, MB + 0x4, 0x9u);          /* data words (not used for dispatch)*/
    wr32(m, MB + 0x10, CMIF_IN_MAGIC);
    wr32(m, MB + 0x14, 0u);           /* version                          */
    wr32(m, MB + 0x18, cmd);
    wr32(m, MB + 0x1C, 0u);           /* token                            */
    return MB + 0x20;
}

/* Response accessors. A moved-handle reply carries the HIPC special header at
 * 0x8 and the moved handle at 0xC, pushing the CMIF out-header to 0x10. */
static uint32_t resp_magic(A64GuestMem *m)  { return rd32(m, MB + 0x10); }
static uint32_t resp_move0(A64GuestMem *m)  { return rd32(m, MB + 0x0C); }

int main(void) {
    int fails = 0;
    A64GuestMem mem;
    A64 cpu;
    HleKernel k;
    HleKernelSession *sess, *ds, *md;
    uint64_t h;
    uint32_t p, dh, mh;

    A64GuestMem_Init(&mem, g_buf, sizeof(g_buf), GUEST_BASE);
    A64GuestMem_Attach(&mem, &cpu);
    HleKernel_Init(&k);
    HleKernel_SetSpanEnd(&k, 0x20000);
    HleKernel_SetLog(&k, my_log, NULL);

    sess = HleKernel_NewSession(&k, "vi:m", HLEK_OBJ_SERVICE);
    CHECK(sess != NULL, "create vi:m session");
    if (!sess) { printf("FAIL: no session\n"); return 1; }
    h = sess->handle;

    /* GetDisplayService (cmd 2): input u32 serviceType -> one moved handle to
     * the IApplicationDisplayService interface. */
    g_log[0] = 0;
    p = build_method(&mem, 2);
    wr32(&mem, p, 1);                 /* serviceType = 1 (system/manager) */
    CHECK(HleIpc_SendSyncRequest(&k, &cpu, h, GUEST_BASE + MB, 0x200) == 1,
          "vi:m GetDisplayService handled");
    CHECK(resp_magic(&mem) == CMIF_OUT_MAGIC, "GetDisplayService CMIF response");
    dh = resp_move0(&mem);
    CHECK(dh != 0, "GetDisplayService returned an interface handle");
    ds = HleKernel_FindSession(&k, dh);
    CHECK(ds != NULL, "returned handle is a live session");
    CHECK(ds && strcmp(ds->name, "IApplicationDisplayService") == 0,
          "returned session is IApplicationDisplayService");
    CHECK(strstr(g_log, "GetDisplayService") != NULL, "logged GetDisplayService");
    CHECK(strstr(g_log, "type=1") != NULL, "logged the service type input");

    /* GetManagerDisplayService (cmd 102) -> IManagerDisplayService
     * (switchbrew: "GetManagerDisplayService Returns an #IManagerDisplayService"). */
    p = build_method(&mem, 102);
    CHECK(HleIpc_SendSyncRequest(&k, &cpu, dh, GUEST_BASE + MB, 0x200) == 1,
          "IApplicationDisplayService GetManagerDisplayService handled");
    CHECK(resp_magic(&mem) == CMIF_OUT_MAGIC, "cmd 102 CMIF response");
    mh = resp_move0(&mem);
    CHECK(mh != 0 && mh != dh, "cmd 102 returned a distinct handle");
    md = HleKernel_FindSession(&k, mh);
    CHECK(md && strcmp(md->name, "IManagerDisplayService") == 0,
          "cmd 102 returns IManagerDisplayService");

    /* GetSystemDisplayService (cmd 101) -> ISystemDisplayService. */
    p = build_method(&mem, 101);
    CHECK(HleIpc_SendSyncRequest(&k, &cpu, dh, GUEST_BASE + MB, 0x200) == 1,
          "IApplicationDisplayService GetSystemDisplayService handled");
    mh = resp_move0(&mem);
    CHECK(mh != 0 && mh != dh, "cmd 101 returned a distinct handle");
    md = HleKernel_FindSession(&k, mh);
    CHECK(md && strcmp(md->name, "ISystemDisplayService") == 0,
          "cmd 101 returns ISystemDisplayService");

    /* GetRelayService (cmd 100) -> IHOSBinderDriver. */
    p = build_method(&mem, 100);
    CHECK(HleIpc_SendSyncRequest(&k, &cpu, dh, GUEST_BASE + MB, 0x200) == 1,
          "IApplicationDisplayService GetRelayService handled");
    mh = resp_move0(&mem);
    CHECK(mh != 0 && mh != dh, "cmd 100 returned a distinct handle");
    md = HleKernel_FindSession(&k, mh);
    CHECK(md && strcmp(md->name, "IHOSBinderDriver") == 0,
          "cmd 100 returns IHOSBinderDriver");

    /* Enumerating/opening a display on the returned interface is NOT
     * implemented: it must stop honestly and name the interface, so the
     * emulator never invents a display. */
    p = build_method(&mem, 1000);     /* ListDisplays */
    CHECK(HleIpc_SendSyncRequest(&k, &cpu, dh, GUEST_BASE + MB, 0x200) == 0,
          "ListDisplays stops honestly");
    CHECK(strstr(HleIpc_LastUnhandled(), "IApplicationDisplayService") != NULL,
          "stop names the display interface");
    CHECK(strstr(HleIpc_LastUnhandled(), "cmd 1000") != NULL,
          "stop names the command");

    /* A second, distinct display call also stops (nothing was "opened"). */
    p = build_method(&mem, 1010);     /* OpenDisplay */
    CHECK(HleIpc_SendSyncRequest(&k, &cpu, dh, GUEST_BASE + MB, 0x200) == 0,
          "OpenDisplay stops honestly");

    /* An unimplemented root command on vi:m itself still stops and is named. */
    p = build_method(&mem, 999);
    CHECK(HleIpc_SendSyncRequest(&k, &cpu, h, GUEST_BASE + MB, 0x200) == 0,
          "unknown vi:m command stops honestly");
    CHECK(strstr(HleIpc_LastUnhandled(), "vi:m") != NULL,
          "stop names vi:m");
    CHECK(strstr(HleIpc_LastUnhandled(), "cmd 999") != NULL,
          "stop names the command");

    if (fails == 0) {
        printf("PASS: vi (vi:m GetDisplayService; display enumeration still an "
               "honest stop)\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", fails);
    return 1;
}
