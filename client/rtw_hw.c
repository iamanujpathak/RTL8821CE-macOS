/*
 * rtw_hw.c — userspace side of the kext ABI: IOConnectCalls to RTW88Server.kext.
 *
 * Since the whole driver moved in-kernel, userspace needs only the high-level
 * control selectors (kBridgeK*: scan/connect/disconnect/status) plus the
 * data-path stats read. The low-level selectors (raw MMIO, DMA alloc, IRQ wait)
 * still exist in the ABI for the in-kernel C shims, but no userspace caller
 * remains, so they are deliberately not wrapped here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>

#include "rtw_hw.h"
#include "../server/rtw88_abi.h"

#define BRIDGE_MAIN_PORT  ((mach_port_t)0)

static io_connect_t g_conn = MACH_PORT_NULL;

/* ---- open / close --------------------------------------------------------- */
int rtw_hw_open(void)
{
    io_service_t svc = IOServiceGetMatchingService(BRIDGE_MAIN_PORT,
                                                   IOServiceMatching("RTW88Server"));
    if (!svc) {
        fprintf(stderr, "rtw_hw: RTW88Server not found — is the kext loaded?\n");
        return -1;
    }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &g_conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "rtw_hw: IOServiceOpen failed 0x%x (run with sudo)\n", kr);
        g_conn = MACH_PORT_NULL;
        return -1;
    }

    /* PING sanity + ABI check. Close on failure — leaking the io_connect_t here
     * used to leak a mach port per retry in rtwd's reconnect loop. */
    uint64_t out[4]; uint32_t nout = 4;
    if (IOConnectCallScalarMethod(g_conn, kBridgePing, NULL, 0, out, &nout) != KERN_SUCCESS) {
        fprintf(stderr, "rtw_hw: PING failed\n");
        rtw_hw_close();
        return -1;
    }
    if (out[0] != RTW_BRIDGE_PING_MAGIC || out[1] != RTW_BRIDGE_ABI_VERSION) {
        fprintf(stderr, "rtw_hw: bad bridge magic/ABI (got 0x%llx v%llu, want v%u) — "
                        "rebuild/reload the kext and the client together\n",
                out[0], out[1], RTW_BRIDGE_ABI_VERSION);
        rtw_hw_close();
        return -1;
    }
    return 0;
}

void rtw_hw_close(void)
{
    if (g_conn != MACH_PORT_NULL) { IOServiceClose(g_conn); g_conn = MACH_PORT_NULL; }
}

/* ---- in-kext data path ---------------------------------------------------- */
void hw_data_stats(uint64_t out[21])
{
    /* struct output: scalar methods cap at 16 values, we return 21 */
    size_t sz = sizeof(uint64_t) * 21;
    for (int i = 0; i < 21; i++) out[i] = 0;
    IOConnectCallStructMethod(g_conn, kBridgeDataStats, NULL, 0, out, &sz);
}

/* ask the kext to run the power-on sequence in-kernel.
 * Returns the kext's (ret<<16)|(CR<<8)|poweron, or ~0 on call failure. */
uint32_t hw_kctl_poweron(void)
{
    uint64_t o[1] = {0}; uint32_t n = 1;
    if (IOConnectCallScalarMethod(g_conn, kBridgeKInit, NULL, 0, o, &n) != KERN_SUCCESS)
        return 0xffffffffu;
    return (uint32_t)o[0];
}

/* full in-kernel bring-up. Returns (ok<<16)|MCUFW_CTRL, ~0 on failure. */
uint32_t hw_kctl_bringup(void)
{
    uint64_t o[1] = {0}; uint32_t n = 1;
    if (IOConnectCallScalarMethod(g_conn, kBridgeKBringup, NULL, 0, o, &n) != KERN_SUCCESS)
        return 0xffffffffu;
    return (uint32_t)o[0];
}

/* in-kernel scan. Fills *r, returns network count (or -1 on failure). */
int hw_kctl_scan(struct rtw_scan_result *r)
{
    size_t sz = sizeof(*r);
    memset(r, 0, sz);
    if (IOConnectCallStructMethod(g_conn, kBridgeKScan, NULL, 0, r, &sz) != KERN_SUCCESS)
        return -1;
    return (int)r->count;
}

/* in-kernel connect. Fills *res (status + card MAC); returns 0 on
 * call success, -1 on call failure. */
int hw_kctl_connect(const char *ssid, const char *pass, struct rtw_connect_result *res)
{
    struct rtw_connect_req req;
    size_t rsz = sizeof(*res);
    memset(&req, 0, sizeof(req));
    memset(res, 0, rsz);
    strncpy(req.ssid, ssid, sizeof(req.ssid) - 1);
    strncpy(req.pass, pass, sizeof(req.pass) - 1);
    if (IOConnectCallStructMethod(g_conn, kBridgeKConnect, &req, sizeof(req), res, &rsz) != KERN_SUCCESS)
        return -1;
    return 0;
}

/* tear down the in-kernel connection (remove enX + free rings). */
int hw_kctl_disconnect(void)
{
    return IOConnectCallScalarMethod(g_conn, kBridgeKDisconnect, NULL, 0, NULL, NULL) == KERN_SUCCESS ? 0 : -1;
}

/* query the live connection state (kext = source of truth). Fills *st; returns 0
 * on call success, -1 on failure. */
int hw_kctl_status(struct rtw_status_result *st)
{
    size_t sz = sizeof(*st);
    memset(st, 0, sz);
    if (IOConnectCallStructMethod(g_conn, kBridgeKStatus, NULL, 0, st, &sz) != KERN_SUCCESS)
        return -1;
    return 0;
}

/* scan a chunk of channels (BEGIN brings the device up, END tears it down). Fills
 * *r with that chunk's networks; returns the count or -1 on call failure. */
int hw_kctl_scan_chunk(const struct rtw_scan_chans *in, struct rtw_scan_result *r)
{
    size_t sz = sizeof(*r);
    memset(r, 0, sz);
    if (IOConnectCallStructMethod(g_conn, kBridgeKScanChunk, in, sizeof(*in), r, &sz) != KERN_SUCCESS)
        return -1;
    return (int)r->count;
}
