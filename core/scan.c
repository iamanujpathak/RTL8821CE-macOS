/*
 * scan.c — interrupt-driven RX scan + beacon parsing.
 *
 * Uses the real rtw88 RX flow: arm MSI via the bridge,
 * unmask RX-OK in the chip IMR, then block on hw_irq_wait() until the chip
 * raises an interrupt, ack the ISR, and drain the RX ring. Each received frame
 * is parsed; beacons/probe-responses yield SSID + BSSID -> a network list.
 *
 * RX descriptor (rx.h): W0 PKT_LEN[13:0], DRV_INFO_SIZE[19:16] (x8 bytes),
 * SHIFT[25:24]. The 802.11 frame sits at desc_sz(24) + shift + drv_info*8.
 */
#ifndef KERNEL
#include <stdio.h>           /* printf: kernel build maps it to IOLog via rtw_shim.h */
#endif
#include <string.h>
#include "trx_regs.h"
#include "trx.h"
#include "config.h"
#include "../server/rtw88_abi.h"   /* rtw_scan_result, for the in-kernel scan marshal */

void set_channel(struct rtw_dev *rtwdev, u8 channel, u8 bw, u8 primary_ch_idx);

#define RTK_PCI_HIMR0 0x0B0
#define RTK_PCI_HISR0 0x0B4
#define IMR_ROK BIT(0)
#define IMR_RDU BIT(1)

#define RX_PKT_DESC_SZ 24

struct net_entry { u8 bssid[6]; char ssid[33]; u8 channel; int beacons; u8 hidden; u8 privacy; };
static struct net_entry g_nets[128];
static int g_nnets;
static int g_probe_resp;   /* probe-responses received = our TX was heard */

/* build a broadcast (wildcard) probe request; returns length */
static u32 build_probe_req(u8 *buf, const u8 *src_mac)
{
    u8 *p = buf;
    *p++ = 0x40; *p++ = 0x00;           /* frame control: mgmt, probe-req(4) */
    *p++ = 0x00; *p++ = 0x00;           /* duration */
    memset(p, 0xff, 6); p += 6;         /* addr1 DA = broadcast */
    memcpy(p, src_mac, 6); p += 6;      /* addr2 SA = our MAC */
    memset(p, 0xff, 6); p += 6;         /* addr3 BSSID = broadcast */
    *p++ = 0x00; *p++ = 0x00;           /* seq ctrl */
    *p++ = 0x00; *p++ = 0x00;           /* SSID IE: wildcard (len 0) */
    *p++ = 0x01; *p++ = 0x08;           /* supported rates IE */
    *p++ = 0x82; *p++ = 0x84; *p++ = 0x8b; *p++ = 0x96;   /* 1,2,5.5,11 */
    *p++ = 0x0c; *p++ = 0x12; *p++ = 0x18; *p++ = 0x24;   /* 6,9,12,18 */
    return (u32)(p - buf);
}

static void add_net(const u8 *bssid, const u8 *ssid, u8 ssid_len, u8 channel, u8 privacy)
{
    for (int i = 0; i < g_nnets; i++) {
        if (memcmp(g_nets[i].bssid, bssid, 6) == 0) { g_nets[i].beacons++; return; }
    }
    if (g_nnets >= (int)(sizeof(g_nets) / sizeof(g_nets[0]))) return;
    struct net_entry *n = &g_nets[g_nnets++];
    memcpy(n->bssid, bssid, 6);
    n->channel = channel;
    n->beacons = 1;
    n->privacy = privacy;
    if (ssid_len > 32) ssid_len = 32;
    int printable = ssid_len > 0;
    for (int k = 0; k < ssid_len; k++) if (ssid[k] == 0) printable = 0;
    n->hidden = !printable;
    if (printable) { memcpy(n->ssid, ssid, ssid_len); n->ssid[ssid_len] = 0; }
    else           { strcpy(n->ssid, "<hidden>"); }
}

static void parse_frame(const u8 *buf, u8 channel)
{
    u32 w0 = *(const u32 *)buf;
    u32 pkt_len  = w0 & 0x3fff;
    u32 drv_info = (w0 >> 16) & 0xf;
    u32 shift    = (w0 >> 24) & 0x3;
    if (w0 & (BIT(14) | BIT(15))) return;      /* HW-flagged CRC32/ICV error — corrupt */
    if (pkt_len < 36 || pkt_len > 2048) return;

    const u8 *f = buf + RX_PKT_DESC_SZ + shift + drv_info * 8;
    u16 fc = (u16)(f[0] | (f[1] << 8));
    u8 type = (fc >> 2) & 0x3, subtype = (fc >> 4) & 0xf;
    if (type != 0) return;                     /* management frames only */
    if (subtype != 8 && subtype != 5) return;  /* beacon (8) or probe-resp (5) */
    if (subtype == 5) g_probe_resp++;          /* a reply to OUR probe = TX works */

    const u8 *bssid = f + 16;                   /* addr3 */
    /* capability field (after tsf8+interval2): bit4 = Privacy (network is encrypted) */
    u8 privacy = (f[24 + 10] & 0x10) ? 1 : 0;
    const u8 *ie = f + 24 + 12;                 /* 802.11 hdr + (tsf8+int2+cap2) */
    const u8 *end = f + pkt_len;
    while (ie + 2 <= end) {
        u8 tag = ie[0], len = ie[1];
        if (ie + 2 + len > end) break;
        if (tag == 0) { add_net(bssid, ie + 2, len, channel, privacy); return; }
        ie += 2 + len;
    }
    add_net(bssid, NULL, 0, channel, privacy);  /* no SSID IE -> hidden */
}

static void drain(struct rtw_dev *rtwdev, u8 channel, u32 *rp)
{
    u32 hw = trx_rx_hw_idx();
    u32 n = trx_rx_count();
    while (*rp != hw) {
        u8 *b = trx_rx_slot_buf(*rp);
        if (b) parse_frame(b, channel);
        *rp = (*rp + 1) % n;
    }
    trx_rx_set_host_idx(rtwdev, *rp);
}

/* Interrupt-driven scan across the given channels; prints the network list. */
int scan_networks(struct rtw_dev *rtwdev, const u8 *channels, int nchan, u32 ms_per)
{
    g_nnets = 0;
    g_probe_resp = 0;

    if (!trx_rx_ok()) { printf("  RX ring not available\n"); return -1; }

    /* arm MSI via the bridge (MAC is on, so the guard allows it) + unmask RX */
    if (hw_irq_enable() != 0) { printf("  hw_irq_enable failed (MAC off?)\n"); return -1; }
    hw_write32(RTK_PCI_HIMR0, IMR_ROK | IMR_RDU);
    printf("  MSI armed + RX unmasked; ACTIVE scan (probe-request TX per channel)\n");

    u8 probe[64];
    u32 plen = build_probe_req(probe, rtwdev->efuse.addr);

    u64 total_irq = 0;
    for (int c = 0; c < nchan; c++) {
        u8 ch = channels[c];
        set_channel(rtwdev, ch, RTW_CHANNEL_WIDTH_20, 0);

        /* skip stale frames: start the read pointer at the current hw index */
        u32 rp = trx_rx_hw_idx();
        trx_rx_set_host_idx(rtwdev, rp);

        /* TX: broadcast probe request (rate: CCK 1M on 2.4GHz, OFDM 6M on 5GHz) */
        u8 rate = (ch <= 14) ? DESC_RATE1M : DESC_RATE6M;
        trx_tx_mgmt(rtwdev, probe, plen, rate, 1);

        int iters = (int)(ms_per / 50);
        for (int i = 0; i < iters; i++) {
            u64 fired = hw_irq_wait(50);        /* block up to 50ms for an MSI */
            total_irq += fired;
            u32 isr = hw_read32(RTK_PCI_HISR0);
            if (isr) hw_write32(RTK_PCI_HISR0, isr);   /* ack (W1C) */
            drain(rtwdev, ch, &rp);             /* process whatever arrived */
            if (i == iters / 2)                 /* re-probe mid-dwell */
                trx_tx_mgmt(rtwdev, probe, plen, rate, 1);
        }
    }
    hw_irq_disable();

    printf("  (%llu MSI interrupts serviced; %d probe-responses to our TX)\n",
           total_irq, g_probe_resp);
    if (g_probe_resp > 0)
        printf("  TX WORKS \xE2\x9C\x93 — APs replied to our probe requests\n");
    printf("\n  %-32s  %-17s  band  ch  beacons\n", "SSID", "BSSID");
    printf("  --------------------------------  -----------------  ----  --  -------\n");
    for (int i = 0; i < g_nnets; i++) {
        struct net_entry *n = &g_nets[i];
        printf("  %-32s  %02x:%02x:%02x:%02x:%02x:%02x  %-4s  %2u  %d\n",
               n->ssid, n->bssid[0], n->bssid[1], n->bssid[2],
               n->bssid[3], n->bssid[4], n->bssid[5],
               n->channel <= 14 ? "2.4" : "5", n->channel, n->beacons);
    }
    return g_nnets;
}

/* copy the scan result list into the ABI struct for the in-kernel scan path
 * (kctl.c -> user-client -> CLI). Returns the number of entries written. */
int scan_marshal(struct rtw_scan_result *r)
{
    int n = g_nnets;
    if (n > RTW_SCAN_MAX) n = RTW_SCAN_MAX;
    r->count = (u32)n;
    for (int i = 0; i < n; i++) {
        memcpy(r->nets[i].bssid, g_nets[i].bssid, 6);
        r->nets[i].channel = g_nets[i].channel;
        r->nets[i].hidden  = g_nets[i].hidden;
        r->nets[i].privacy = g_nets[i].privacy;
        memcpy(r->nets[i].ssid, g_nets[i].ssid, 33);
        r->nets[i].beacons = (u32)g_nets[i].beacons;
    }
    return n;
}

/* find a scanned network by exact SSID -> its BSSID + channel (the in-kernel
 * connect passes the SSID from the CLI). Returns 1 if found. */
int find_by_ssid(const char *ssid, u8 *bssid_out, u8 *channel_out)
{
    for (int i = 0; i < g_nnets; i++) {
        if (!g_nets[i].hidden && strcmp(g_nets[i].ssid, ssid) == 0) {
            memcpy(bssid_out, g_nets[i].bssid, 6);
            *channel_out = g_nets[i].channel;
            return 1;
        }
    }
    return 0;
}

/* pick a join target: the strongest scanned network that matches one of the
 * configured known networks (so the same config roams across locations).
 * Returns 1 if one was found. */
int best_target(u8 *bssid, char *ssid, u8 *ssid_len, u8 *channel)
{
    struct net_entry *best = NULL;
    for (int i = 0; i < g_nnets; i++) {
        if (g_nets[i].hidden) continue;
        if (!network_match(g_nets[i].ssid)) continue;   /* only known networks */
        if (!best || g_nets[i].beacons > best->beacons) best = &g_nets[i];
    }
    if (!best) return 0;
    memcpy(bssid, best->bssid, 6);
    strcpy(ssid, best->ssid);
    *ssid_len = (u8)strlen(best->ssid);
    *channel = best->channel;
    return 1;
}
