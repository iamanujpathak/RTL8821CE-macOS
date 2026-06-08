/*
 * rtw_hw.c — bridge-backed implementation of the userspace HW layer.
 * Every register/DMA/IRQ op is an IOConnectCall to RTW88Server.kext.
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
        return -1;
    }

    /* PING sanity + ABI check */
    uint64_t out[4]; uint32_t nout = 4;
    if (IOConnectCallScalarMethod(g_conn, kBridgePing, NULL, 0, out, &nout) != KERN_SUCCESS) {
        fprintf(stderr, "rtw_hw: PING failed\n"); return -1;
    }
    if (out[0] != RTW_BRIDGE_PING_MAGIC || out[1] != RTW_BRIDGE_ABI_VERSION) {
        fprintf(stderr, "rtw_hw: bad bridge magic/ABI (got 0x%llx v%llu)\n", out[0], out[1]);
        return -1;
    }
    return 0;
}

void rtw_hw_close(void)
{
    if (g_conn != MACH_PORT_NULL) { IOServiceClose(g_conn); g_conn = MACH_PORT_NULL; }
}

void rtw_hw_ident(uint16_t *vendor, uint16_t *device, uint32_t *bar_len)
{
    uint64_t out[4]; uint32_t nout = 4;
    IOConnectCallScalarMethod(g_conn, kBridgePing, NULL, 0, out, &nout);
    if (vendor)  *vendor  = (out[2] >> 16) & 0xffff;
    if (device)  *device  = out[2] & 0xffff;
    if (bar_len) *bar_len = (uint32_t)out[3];
}

/* ---- register access ------------------------------------------------------ */
static uint64_t reg_read(uint32_t off, uint32_t width)
{
    uint64_t in[2] = { off, width }, out[1] = { 0 }; uint32_t nout = 1;
    IOConnectCallScalarMethod(g_conn, kBridgeRegRead, in, 2, out, &nout);
    return out[0];
}
static void reg_write(uint32_t off, uint32_t width, uint64_t val)
{
    uint64_t in[3] = { off, width, val };
    IOConnectCallScalarMethod(g_conn, kBridgeRegWrite, in, 3, NULL, NULL);
}

uint8_t  hw_read8 (uint32_t off) { return (uint8_t) reg_read(off, 1); }
uint16_t hw_read16(uint32_t off) { return (uint16_t)reg_read(off, 2); }
uint32_t hw_read32(uint32_t off) { return (uint32_t)reg_read(off, 4); }
void hw_write8 (uint32_t off, uint8_t  v) { reg_write(off, 1, v); }
void hw_write16(uint32_t off, uint16_t v) { reg_write(off, 2, v); }
void hw_write32(uint32_t off, uint32_t v) { reg_write(off, 4, v); }

uint32_t hw_cfg_read32(uint32_t off)
{
    uint64_t in[2] = { off, 4 }, out[1] = { 0 }; uint32_t nout = 1;
    IOConnectCallScalarMethod(g_conn, kBridgeCfgRead, in, 2, out, &nout);
    return (uint32_t)out[0];
}

/* ---- power ---------------------------------------------------------------- */
void hw_power(bool mem_enable, bool busmaster)
{
    uint64_t in[2] = { mem_enable, busmaster };
    IOConnectCallScalarMethod(g_conn, kBridgePower, in, 2, NULL, NULL);
}
void hw_set_mac_power(bool on)
{
    uint64_t in[1] = { on };
    IOConnectCallScalarMethod(g_conn, kBridgeSetMacPower, in, 1, NULL, NULL);
}

/* ---- DMA ------------------------------------------------------------------ */
int hw_dma_alloc(uint64_t size, uint64_t *handle, uint64_t *paddr, void **vaddr)
{
    uint64_t in[1] = { size }, out[3] = { 0 }; uint32_t nout = 3;
    if (IOConnectCallScalarMethod(g_conn, kBridgeDmaAlloc, in, 1, out, &nout) != KERN_SUCCESS)
        return -1;
    uint64_t h = out[0], pa = out[1] | (out[2] << 32);
    mach_vm_address_t addr = 0; mach_vm_size_t msz = 0;
    if (IOConnectMapMemory64(g_conn, (uint32_t)h, mach_task_self(), &addr, &msz,
                             kIOMapAnywhere) != KERN_SUCCESS)
        return -1;
    if (handle) *handle = h;
    if (paddr)  *paddr  = pa;
    if (vaddr)  *vaddr  = (void *)addr;
    return 0;
}
void hw_dma_free(uint64_t handle, void *vaddr, uint64_t size)
{
    (void)size;
    if (vaddr) IOConnectUnmapMemory64(g_conn, (uint32_t)handle, mach_task_self(),
                                      (mach_vm_address_t)vaddr);
    uint64_t in[1] = { handle };
    IOConnectCallScalarMethod(g_conn, kBridgeDmaFree, in, 1, NULL, NULL);
}
int hw_reg_write_dma(uint32_t reg_off, uint64_t handle, uint64_t buf_off, uint32_t width)
{
    uint64_t in[4] = { reg_off, handle, buf_off, width };
    return IOConnectCallScalarMethod(g_conn, kBridgeRegWriteDma, in, 4, NULL, NULL) == KERN_SUCCESS ? 0 : -1;
}

/* ---- interrupts ----------------------------------------------------------- */
int hw_irq_enable(void)
{
    return IOConnectCallScalarMethod(g_conn, kBridgeIrqEnable, NULL, 0, NULL, NULL) == KERN_SUCCESS ? 0 : -1;
}
void hw_irq_disable(void)
{
    IOConnectCallScalarMethod(g_conn, kBridgeIrqDisable, NULL, 0, NULL, NULL);
}
uint64_t hw_irq_wait(uint32_t timeout_ms)
{
    uint64_t in[1] = { timeout_ms }, out[1] = { 0 }; uint32_t nout = 1;
    IOConnectCallScalarMethod(g_conn, kBridgeIrqWait, in, 1, out, &nout);
    return out[0];
}
int hw_irq_status(uint32_t off, uint32_t width, uint32_t *val)
{
    uint64_t in[2] = { off, width }, out[1] = { 0 }; uint32_t nout = 1;
    kern_return_t kr = IOConnectCallScalarMethod(g_conn, kBridgeIrqStatus, in, 2, out, &nout);
    if (val) *val = (uint32_t)out[0];
    return kr == KERN_SUCCESS ? 0 : (int)kr;
}

/* ---- in-kext data path ---------------------------------------------------- */
int hw_data_start(const struct rtw_data_cfg *cfg)
{
    return IOConnectCallStructMethod(g_conn, kBridgeDataStart, cfg, sizeof(*cfg),
                                     NULL, NULL) == KERN_SUCCESS ? 0 : -1;
}
void hw_data_stop(void)
{
    IOConnectCallScalarMethod(g_conn, kBridgeDataStop, NULL, 0, NULL, NULL);
}
void hw_data_link(int up)
{
    uint64_t in[1] = { up != 0 };
    IOConnectCallScalarMethod(g_conn, kBridgeDataLink, in, 1, NULL, NULL);
}
void hw_data_stats(uint64_t out[21])
{
    /* struct output: scalar methods cap at 16 values, we return 20 */
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
