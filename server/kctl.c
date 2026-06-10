/*
 * kctl.c — in-kernel control entry points. The 802.11
 * bring-up C (pwr.c etc., lifted from upstream rtw88) is compiled into the
 * kext and runs in-kernel here, hitting the hardware directly instead of routing
 * every MMIO access through the userspace<->kext ABI.
 *
 * The C++ RTW88Server owns the PCI device + BAR mapping; it publishes the BAR2
 * base into g_kmmio once mapped, and the small MMIO shim below (what the
 * code's rtw_read8/rtw_write8 macros resolve to) reads/writes through it. This is
 * the only seam between the C control code and the C++ IOKit object.
 */
#include "rtw_shim.h"   /* struct rtw_dev/chip_info, enums, hw_ decls, upstream/reg.h */
#include "config.h"     /* struct wifi_session */
#include "rtw88_abi.h"  /* rtw_scan_result */

/* g_session (the TX descriptor raid, set at association) is now provided by assoc.c,
 * compiled into the kext for connect. */

/* scan.c's best_target() (unused in-kernel — the CLI passes SSID/pass directly)
 * references this config.c helper; stub it so the kext links without pulling in the
 * userspace config/arg-parsing machinery. */
const struct wifi_network *network_match(const char *ssid) { (void)ssid; return 0; }

/* BAR2 base — set by RTW88Server::start() once mapDeviceMemory succeeds. */
volatile uint8_t *g_kmmio = 0;

/* Live connection state — the single source of truth for "are we connected and to
 * what", set by rtw_kctl_connect (whoever called it: CLI, rtwd, auto) and cleared
 * by rtw_kctl_disconnect. rtw_kctl_status() reports it so the GUI stays in sync. */
static int     g_cur_connected = 0;
static char    g_cur_ssid[33]  = "";
static uint8_t g_cur_bssid[6]  = {0};
static uint8_t g_cur_mac[6]    = {0};
static uint8_t g_cur_channel   = 0;

/* Streamed-scan state: the device is brought up ONCE (BEGIN) and kept up across
 * chunk calls so the daemon can scan a few channels at a time and stream results,
 * then torn down (END). g_scan_dev persists across the BEGIN/chunk/END calls. */
static struct rtw_dev g_scan_dev;
static int            g_scan_active = 0;

/* ---- MMIO shim: the control C lands here (rtw_read8 -> hw_read8 ...) ---- */
uint8_t  hw_read8 (uint32_t off) { return g_kmmio ? g_kmmio[off] : (uint8_t)0xff; }
uint16_t hw_read16(uint32_t off) { return g_kmmio ? *(volatile uint16_t *)(g_kmmio + off) : (uint16_t)0xffff; }
uint32_t hw_read32(uint32_t off) { return g_kmmio ? *(volatile uint32_t *)(g_kmmio + off) : 0xffffffffu; }
void hw_write8 (uint32_t off, uint8_t  v) { if (g_kmmio) g_kmmio[off] = v; }
void hw_write16(uint32_t off, uint16_t v) { if (g_kmmio) *(volatile uint16_t *)(g_kmmio + off) = v; }
void hw_write32(uint32_t off, uint32_t v) { if (g_kmmio) *(volatile uint32_t *)(g_kmmio + off) = v; }

/* ---- chip descriptor: only the fields the power-on sequencer reads ---- */
extern const struct rtw_pwr_seq_cmd * const card_enable_flow_8821c[];
extern const struct rtw_pwr_seq_cmd * const card_disable_flow_8821c[];
extern int rtw_mac_power_on(struct rtw_dev *rtwdev);

/* The full 8821c chip descriptor (mirrors client/main.c's chip_8821c). The page/rqpn
 * tables live in macinit.c (also compiled into the kext). */
extern const struct rtw_page_table page_table_8821c[];
extern const struct rtw_rqpn       rqpn_table_8821c[];

static const struct rtw_chip_info chip_8821c = {
    .id          = RTW_CHIP_TYPE_8821C,
    .wlan_cpu    = RTW_WCPU_3081,
    .sys_func_en = 0xD8,
    .pwr_on_seq  = card_enable_flow_8821c,
    .pwr_off_seq = card_disable_flow_8821c,
    .tx_pkt_desc_sz = 48,
    .tx_buf_desc_sz = 16,
    .rx_pkt_desc_sz = 24,
    .rx_buf_desc_sz = 8,
    .ltecoex_addr   = 0,
    .txff_size      = 65536,
    .rxff_size      = 16384,
    .rsvd_drv_pg_num = 8,
    .csi_buf_pg_num  = 0,
    .page_size       = 128,
    .page_table      = page_table_8821c,
    .rqpn_table      = rqpn_table_8821c,
    .phy_efuse_size  = 512,
    .log_efuse_size  = 512,
    .ptct_efuse_size = 96,
};

static void kdev_init(struct rtw_dev *d)
{
    memset(d, 0, sizeof(*d));
    d->chip = &chip_8821c;
    d->hci.type = RTW_HCI_TYPE_PCIE;
    d->hci.rpwm_addr = 0x03d9;   /* PCIe RPWM */
    d->hal.cut_version = 0;
    d->hal.rf_path_num = 1;      /* 8821c is 1T1R — MUST be set before phy_set_param, or */
    d->hal.rf_phy_num  = 1;      /* rtw_phy_read/write_rf early-return INV (rf_path>=num) */
}

/* run the power-on sequence in-kernel and report the result.
 * Returns (ret<<16)|(CR<<8)|poweron. CR==0x00 + poweron==1 means the MAC came up. */
uint32_t rtw_kctl_poweron(void)
{
    struct rtw_dev d;
    kdev_init(&d);
    int ret = rtw_mac_power_on(&d);
    uint8_t cr = hw_read8(REG_CR);
    int poweron = test_bit(RTW_FLAG_POWERON, d.flags) ? 1 : 0;
    IOLog("RTW88 kctl: in-kernel power-on ret=%d CR=0x%02x poweron=%d\n", ret, cr, poweron);
    return ((uint32_t)(ret & 0xff) << 16) | ((uint32_t)cr << 8) | (uint32_t)poweron;
}

/* the full bring-up chain in-kernel (the same sequence main.c runs):
 * power-on -> TRX rings -> firmware download -> mac_init -> efuse -> phy_set_param.
 * Returns (ok<<16)|MCUFW_CTRL. ok==1 needs every stage to return 0 and the firmware
 * to report ready (MCUFW_CTRL ~0xC078, FWDL bits set) — same checkpoints as userspace. */
extern int trx_init(struct rtw_dev *rtwdev);
extern int download_firmware(struct rtw_dev *rtwdev, const unsigned char *data, unsigned int size);
extern int mac_init(struct rtw_dev *rtwdev);
extern int efuse_read(struct rtw_dev *rtwdev);
extern int phy_set_param(struct rtw_dev *rtwdev);
extern const unsigned char rtw8821c_fw_data[];
extern unsigned int rtw8821c_fw_data_len;
extern void trx_free(void);   /* frees the DMA rings + halts DMA (avoids leaking on each call) */

uint32_t rtw_kctl_bringup(void)
{
    struct rtw_dev d;
    kdev_init(&d);

    int pr = rtw_mac_power_on(&d);
    hw_set_mac_power(1);   /* allow irqEnable() (not used by bringup, but keep the flag honest) */
    int tr = trx_init(&d);
    int fr = download_firmware(&d, rtw8821c_fw_data, rtw8821c_fw_data_len);
    uint16_t mcufw = hw_read16(REG_MCUFW_CTRL);
    int mr = mac_init(&d);
    int er = efuse_read(&d);
    int pe = phy_set_param(&d);

    const uint8_t *m = d.efuse.addr;
    IOLog("RTW88 kctl: bringup pwr=%d trx=%d fw=%d(mcufw=0x%04x) mac=%d efuse=%d phy=%d "
          "MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
          pr, tr, fr, mcufw, mr, er, pe, m[0], m[1], m[2], m[3], m[4], m[5]);

    int ok = (pr == 0 && tr == 0 && fr == 0 && mr == 0 && er == 0 && pe == 0 && (mcufw & 0xc000));
    trx_free();   /* release the DMA rings (status already captured above) */
    return ((uint32_t)(ok ? 1 : 0) << 16) | mcufw;
}

/* full bring-up + fw H2C handshake + antenna grant + in-kernel scan, marshalled
 * into `out`. Returns the network count (also logged). Same bring-up sequence as
 * main.c runs, then the interrupt-driven scan. */
extern void fw_handshake(struct rtw_dev *rtwdev);
extern void coex_wifi_antenna(struct rtw_dev *rtwdev);
extern int  scan_networks(struct rtw_dev *rtwdev, const unsigned char *channels, int nchan, unsigned int ms_per);
extern int  scan_marshal(struct rtw_scan_result *r);

uint32_t rtw_kctl_scan(struct rtw_scan_result *out)
{
    static const unsigned char chans[] = {
        36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128,
        132, 136, 140, 149, 153, 157, 161, 165,                 /* 5 GHz */
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13               /* 2.4 GHz */
    };
    struct rtw_dev d;
    kdev_init(&d);

    /* Same preamble as connect/scan-chunk BEGIN: a previous scan/connect ended in
     * trx_free(), which disables PCI bus-mastering — without re-enabling it here every
     * scan after the first is DMA-dead (probe requests never TX, the RX ring never
     * fills, 0 networks; rtwd's auto-connect then wedges forever). Also tear down any
     * live data path first: re-running power-on under live RX DMA is the no-IOMMU
     * corruption hazard. */
    hw_data_stop();
    trx_free();
    g_scan_active   = 0;
    g_cur_connected = 0;
    hw_power(1, 1);

    int pr = rtw_mac_power_on(&d);
    hw_set_mac_power(1);   /* gate: lets scan_networks' hw_irq_enable() succeed */
    int tr = trx_init(&d);
    int fr = download_firmware(&d, rtw8821c_fw_data, rtw8821c_fw_data_len);
    uint16_t mcufw = hw_read16(REG_MCUFW_CTRL);
    int mr = mac_init(&d);
    int er = efuse_read(&d);
    int pe = phy_set_param(&d);
    extern int trx_rx_ok(void);
    int rxok = trx_rx_ok();
    const uint8_t *m = d.efuse.addr;
    IOLog("RTW88 kctl: scan pre-stage pwr=%d trx=%d fw=%d(mcufw=0x%04x) mac=%d efuse=%d phy=%d "
          "rxok=%d MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
          pr, tr, fr, mcufw, mr, er, pe, rxok, m[0], m[1], m[2], m[3], m[4], m[5]);

    fw_handshake(&d);
    /* hal.rfe_btg is derived from efuse.rfe_option in efuse_read() above — it
     * selects the 2.4GHz RF front-end (BTG vs WLG). Do NOT clobber it here. */
    coex_wifi_antenna(&d);

    int n = scan_networks(&d, chans, (int)sizeof(chans), 300);
    if (n < 0) n = 0;
    scan_marshal(out);
    IOLog("RTW88 kctl: in-kernel scan found %d networks\n", n);
    trx_free();   /* release the DMA rings (don't leak ~1MB of wired contiguous per call) */
    return (uint32_t)n;
}

/* Chunked scan: bring the device up once (BEGIN), scan a subset of channels per
 * call (streaming the results out to the daemon), then tear down (END). Keeps the
 * device up across calls in g_scan_dev so bring-up isn't repeated per chunk. */
void rtw_kctl_scan_chunk(const struct rtw_scan_chans *in, struct rtw_scan_result *out)
{
    memset(out, 0, sizeof(*out));

    if (in->flags & RTW_SCAN_F_BEGIN) {
        /* same no-IOMMU hazard as connect: a scan while connected would reallocate
         * the rings + reset the MAC under the live data path. Tear it down first. */
        hw_data_stop();
        trx_free();                      /* halt DMA + free any prior scan/connection rings */
        g_scan_active   = 0;
        g_cur_connected = 0;
        kdev_init(&g_scan_dev);
        hw_power(1, 1);                  /* re-enable PCI mem + bus-mastering (prior trx_free off'd it) */
        rtw_mac_power_on(&g_scan_dev);
        hw_set_mac_power(1);
        trx_init(&g_scan_dev);
        download_firmware(&g_scan_dev, rtw8821c_fw_data, rtw8821c_fw_data_len);
        mac_init(&g_scan_dev);
        efuse_read(&g_scan_dev);
        phy_set_param(&g_scan_dev);
        fw_handshake(&g_scan_dev);
        coex_wifi_antenna(&g_scan_dev);
        g_scan_active = 1;
        IOLog("RTW88 kctl: scan BEGIN (device up)\n");
    }

    if (g_scan_active && in->count) {
        unsigned cnt = in->count > sizeof(in->ch) ? (unsigned)sizeof(in->ch) : in->count;
        int n = scan_networks(&g_scan_dev, in->ch, (int)cnt, 250);
        if (n < 0) n = 0;
        scan_marshal(out);   /* this chunk's networks (scan_networks resets the list per call) */
    }

    if (in->flags & RTW_SCAN_F_END) {
        if (g_scan_active) { trx_free(); g_scan_active = 0; }
        IOLog("RTW88 kctl: scan END (device down)\n");
    }
}

/* full in-kernel CONNECT — bring-up + scan (to locate the SSID) + 802.11 auth +
 * association + WPA2 4-way + key install + media-connect, the same chain main.c runs
 * (the data-path handoff tail is #ifndef KERNEL'd out in wpa.c). On success the
 * device is left associated + keyed. Returns found|associated<<1|wpa<<2|chan<<8. */
extern int associate(struct rtw_dev *rtwdev, const unsigned char *bssid,
                           const char *ssid, unsigned char ssid_len, unsigned char channel, const char *pass);
extern int find_by_ssid(const char *ssid, unsigned char *bssid_out, unsigned char *channel_out);
extern int g_wpa_done;

void rtw_kctl_connect(const struct rtw_connect_req *req, struct rtw_connect_result *out)
{
    static const unsigned char chans[] = {
        36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128,
        132, 136, 140, 149, 153, 157, 161, 165, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13
    };
    memset(out, 0, sizeof(*out));

    /* The struct arrives as a verbatim copy-in from userspace: nothing guarantees the
     * ssid/pass arrays are NUL-terminated, and both are strlen()'d downstream (PBKDF2
     * runs strlen(pass)) — force-terminate a local copy so a 64-byte unterminated pass
     * can't walk kernel heap. */
    struct rtw_connect_req rq = *req;
    rq.ssid[sizeof(rq.ssid) - 1] = 0;
    rq.pass[sizeof(rq.pass) - 1] = 0;
    req = &rq;

    /* CRITICAL (no-IOMMU): tear down any data path / scan from a PREVIOUS connection
     * BEFORE re-running power-on. The GUI/auto-connect can issue a connect while already
     * connected (e.g. switching 5GHz->2.4GHz). The DMA rings are allocate-once and are
     * NOT reallocated here (trx_init early-returns and just reprograms the ring
     * registers) — the hazard is the LIVE old RX thread + in-flight DMA when the
     * power-on sequence resets the MAC underneath them. hw_data_stop() joins the old RX
     * thread; trx_free() then halts TRX DMA + disables bus-mastering (it never frees the
     * buffers). Both are safe no-ops if nothing is running. */
    hw_data_stop();
    trx_free();
    g_scan_active   = 0;
    g_cur_connected = 0;

    struct rtw_dev d;
    kdev_init(&d);
    hw_power(1, 1);   /* re-enable PCI mem + bus-mastering (the teardown above turned it off) */

    /* Bring-up, stage by stage. A failed stage leaves the chip half-programmed with
     * bus-mastering on, so abort — and trx_free() to halt DMA + drop bus-mastering —
     * rather than scan/associate against an undefined device. out->status stays 0. */
    int pr = rtw_mac_power_on(&d);
    hw_set_mac_power(1);
    int tr = (pr == 0) ? trx_init(&d) : -1;
    int fr = (tr == 0) ? download_firmware(&d, rtw8821c_fw_data, rtw8821c_fw_data_len) : -1;
    int mr = (fr == 0) ? mac_init(&d) : -1;
    int er = (mr == 0) ? efuse_read(&d) : -1;
    int pe = (er == 0) ? phy_set_param(&d) : -1;
    if (pr || tr || fr || mr || er || pe) {
        IOLog("RTW88 kctl: connect bring-up FAILED (pwr=%d trx=%d fw=%d mac=%d efuse=%d phy=%d) — aborting\n",
              pr, tr, fr, mr, er, pe);
        trx_free();
        return;   /* out->status stays 0 (found=0) */
    }
    fw_handshake(&d);
    /* hal.rfe_btg is derived from efuse.rfe_option in efuse_read() above — it
     * selects the 2.4GHz RF front-end (BTG vs WLG). Do NOT clobber it here. */
    coex_wifi_antenna(&d);

    scan_networks(&d, chans, (int)sizeof(chans), 300);

    unsigned char bssid[6], channel = 0;
    if (!find_by_ssid(req->ssid, bssid, &channel)) {
        IOLog("RTW88 kctl: connect — SSID '%s' not found in scan\n", req->ssid);
        trx_free();
        return;   /* out->status stays 0 (found=0) */
    }
    out->channel = channel;

    g_wpa_done = 0;
    int ar = associate(&d, bssid, req->ssid, (unsigned char)strlen(req->ssid), channel, req->pass);
    int associated = (ar == 0);
    int wpa = g_wpa_done ? 1 : 0;
    IOLog("RTW88 kctl: connect '%s' ch%u -> associated=%d wpa_done=%d\n",
          req->ssid, channel, associated, wpa);

    if (associated && wpa) {
        /* start the in-kernel data path on the (kext-allocated) trx rings — same
         * cfg the userspace eth path fills, but built + started in-kernel. Publishes
         * enX; macOS/configd then DHCPs it (L3 stays a userspace/configd concern). */
        extern void trx_fill_data_cfg(struct rtw_dev *rtwdev, struct rtw_data_cfg *cfg);
        struct rtw_data_cfg cfg;
        trx_fill_data_cfg(&d, &cfg);
        memcpy(cfg.mac, d.efuse.addr, 6);
        memcpy(cfg.bssid, bssid, 6);
        cfg.rate    = g_session.init_rate;
        cfg.rate_id = g_session.raid;
        int data_up = (hw_data_start(&cfg) == 0);
        if (data_up) {
            hw_data_link(1); IOLog("RTW88 kctl: in-kernel data path UP (enX)\n");
            /* publish the live connection state (single source of truth for status) */
            g_cur_connected = 1;
            strncpy(g_cur_ssid, req->ssid, sizeof(g_cur_ssid) - 1);
            g_cur_ssid[sizeof(g_cur_ssid) - 1] = 0;
            memcpy(g_cur_bssid, bssid, 6);
            memcpy(g_cur_mac, d.efuse.addr, 6);
            g_cur_channel = channel;
        }
        else         IOLog("RTW88 kctl: hw_data_start FAILED\n");
        memcpy(out->mac, d.efuse.addr, 6);
        out->status = 1u | ((uint32_t)associated << 1) | ((uint32_t)wpa << 2) | ((uint32_t)data_up << 3);
    } else {
        trx_free();   /* not connected -> release the rings */
        out->status = 1u | ((uint32_t)associated << 1) | ((uint32_t)wpa << 2);
    }
}

/* tear down the in-kernel connection — remove enX (so macOS falls back to other
 * interfaces) and release the DMA rings. A later --kconnect re-runs the full bring-up
 * (it re-enables bus-mastering, which trx_free turns off). */
extern int trx_tx_mgmt(struct rtw_dev *rtwdev, const unsigned char *frame, unsigned int len,
                       unsigned char rate, int bmc);

void rtw_kctl_disconnect(void)
{
    /* Tell the AP we are leaving (deauth, reason 3 "STA is leaving") BEFORE tearing the
     * rings down, so it frees our association state instead of retrying a dead peer
     * until its inactivity timer fires. Uses the MGMT ring, which the in-kext data path
     * does not touch, so it is safe while the data path is still up. Best-effort. */
    if (g_cur_connected && g_kmmio) {
        unsigned char f[26];
        f[0] = 0xc0; f[1] = 0x00;                 /* mgmt / deauthentication */
        f[2] = 0;    f[3] = 0;                    /* duration */
        memcpy(f + 4,  g_cur_bssid, 6);           /* addr1 = AP   */
        memcpy(f + 10, g_cur_mac,   6);           /* addr2 = us   */
        memcpy(f + 16, g_cur_bssid, 6);           /* addr3 = BSSID */
        f[22] = 0; f[23] = 0;                     /* seq (HW fills) */
        f[24] = 3; f[25] = 0;                     /* reason 3: deauth, leaving */
        struct rtw_dev d;
        kdev_init(&d);
        trx_tx_mgmt(&d, f, sizeof(f), g_cur_channel > 14 ? 0x04 /*6M*/ : 0x00 /*1M*/, 0);
        usleep_range(20000, 20000);               /* let it air out before halting DMA */
    }
    hw_data_stop();   /* remove enX + stop the RX thread */
    trx_free();       /* halt TRX DMA, disable bus-mastering, free the rings */
    g_cur_connected = 0;
    g_cur_ssid[0] = 0;
    IOLog("RTW88 kctl: disconnected — enX removed, rings freed\n");
}

/* clear all in-kernel control state. Called from RTW88Server::stop() because the
 * kctl statics outlive a single device instance (the kext stays loaded across a
 * provider stop()/start() re-probe): a stale g_cur_connected would make status
 * report a phantom connection and let the next disconnect deauth through freed
 * trx buffers. */
void rtw_kctl_unload_reset(void)
{
    g_cur_connected = 0;
    g_cur_ssid[0] = 0;
    memset(g_cur_bssid, 0, sizeof(g_cur_bssid));
    memset(g_cur_mac, 0, sizeof(g_cur_mac));
    g_cur_channel = 0;
    g_scan_active = 0;
}

/* report the live connection state (set by the last connect, cleared by
 * disconnect). The kext is authoritative, so a CLI-initiated connect is visible
 * to rtwd/GUI here even though it never went through rtwd. */
void rtw_kctl_status(struct rtw_status_result *out)
{
    memset(out, 0, sizeof(*out));
    out->connected = g_cur_connected ? 1 : 0;
    if (g_cur_connected) {
        memcpy(out->mac, g_cur_mac, 6);
        memcpy(out->bssid, g_cur_bssid, 6);
        out->channel = g_cur_channel;
        strncpy(out->ssid, g_cur_ssid, sizeof(out->ssid) - 1);
    }
}
