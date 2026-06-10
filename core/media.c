/*
 * media.c — media-status-report ("we are connected").
 *
 * After association + key install, tell the firmware that our peer MACID is
 * connected. This is the rtw88 rtw_fw_media_status_report H2C *command* (8-byte
 * mailbox, not the 32-byte H2C-queue packet): once the firmware marks the MACID
 * connected it runs auto-ACK + rate adaptation for that peer, so unicast data
 * frames we receive get ACKed (otherwise the AP retries the DHCP offer and gives
 * up). Also program the hardware BSSID filter so RX keeps frames from our AP.
 *
 * Verbatim command layout from upstream rtw88 fw.h / fw.c.
 */
#ifndef KERNEL
#include <stdio.h>
#endif
#include "trx_regs.h"
#include "config.h"

/* H2C mailbox command (rtw_fw_send_h2c_command) */
#define H2C_CMD_MEDIA_STATUS_RPT  0x01
#define H2C_CMD_RA_INFO           0x40   /* rtw_fw_send_ra_info — per-MACID rate adaptation */
#define REG_MACID                 0x0610 /* port-0 self-MAC; HW auto-ACKs unicast w/ RA==this */
#define REG_BSSID                 0x0618
#define REG_MSR                   0x0102 /* media status: port-0 network type in bits[1:0]   */
#define REG_RRSR                  0x0440 /* response-rate set: rates HW uses for ACK/BA/CTS  */
#define MSR_NETTYPE_INFRA         0x02   /* RTW_NET_MGD_LINKED — associated infrastructure STA */

/* one H2C command via the next free mailbox (REG_HMEBOX0..3 + _EX) */
static void h2c_command(struct rtw_dev *rtwdev, const u8 *h2c)
{
    static int box_num = 0;
    u32 box_reg, box_ex_reg;
    switch (box_num) {
    default:
    case 0: box_reg = REG_HMEBOX0; box_ex_reg = REG_HMEBOX0_EX; break;
    case 1: box_reg = REG_HMEBOX1; box_ex_reg = REG_HMEBOX1_EX; break;
    case 2: box_reg = REG_HMEBOX2; box_ex_reg = REG_HMEBOX2_EX; break;
    case 3: box_reg = REG_HMEBOX3; box_ex_reg = REG_HMEBOX3_EX; break;
    }
    /* wait for the box to be empty (REG_HMETFR bit per box clears) */
    for (int i = 0; i < 100; i++) {
        if (!(hw_read8(REG_HMETFR) & BIT(box_num))) break;
        usleep_range(1000, 1000);
    }
    /* write the ex (upper) dword first, then the trigger dword */
    u32 lo = h2c[0] | ((u32)h2c[1] << 8) | ((u32)h2c[2] << 16) | ((u32)h2c[3] << 24);
    u32 hi = h2c[4] | ((u32)h2c[5] << 8) | ((u32)h2c[6] << 16) | ((u32)h2c[7] << 24);
    hw_write32(box_ex_reg, hi);
    hw_write32(box_reg, lo);
    box_num = (box_num + 1) % 4;
}

/* Program firmware per-MACID rate adaptation (rtw_fw_send_ra_info, H2C 0x40).
 * Without this the firmware has no rate table for our peer, so unless we pin the
 * rate in the TX descriptor it transmits at the lowest default. Layout (word0):
 *   [7:0]=cmd  [15:8]=macid  [20:16]=rate_id(raid)  [22:21]=init_ra_lvl
 *   [23]=sgi   [25:24]=bw    [26]=ldpc  [27]=no_update  [29:28]=vht_en  [30]=dis_pt
 * word1 = ra_mask (which rates the firmware may pick). When the assoc req only
 * advertises legacy caps the mask is legacy/OFDM only; HT/VHT bits get OR'd in
 * once we advertise those caps in the assoc req. */
static void fw_ra_info(struct rtw_dev *rtwdev, u8 mac_id, u8 rate_id, u32 ra_mask,
                       u8 bw, u8 sgi, u8 vht_en)
{
    u8 h2c[8] = {0};
    u32 w0 = (u32)H2C_CMD_RA_INFO          /* [7:0]  */
           | ((u32)mac_id  & 0xff)  << 8   /* [15:8] */
           | ((u32)rate_id & 0x1f)  << 16  /* [20:16] */
           | ((u32)0       & 0x03)  << 21  /* init_ra_lvl */
           | ((u32)(sgi ? 1 : 0))   << 23
           | ((u32)bw      & 0x03)  << 24
           | ((u32)0)               << 26  /* ldpc */
           | ((u32)0)               << 27  /* no_update: 0 = reset RA table now */
           | ((u32)vht_en  & 0x03)  << 28
           | ((u32)1)               << 30; /* dis_pt = 1 (upstream always) */
    h2c[0] = (u8)w0;       h2c[1] = (u8)(w0 >> 8);
    h2c[2] = (u8)(w0 >> 16); h2c[3] = (u8)(w0 >> 24);
    h2c[4] = (u8)ra_mask;       h2c[5] = (u8)(ra_mask >> 8);
    h2c[6] = (u8)(ra_mask >> 16); h2c[7] = (u8)(ra_mask >> 24);
    h2c_command(rtwdev, h2c);
    usleep_range(5000, 5000);
}

void media_connect(struct rtw_dev *rtwdev, const u8 *bssid, u8 mac_id, u8 channel)
{
    /* port-0 self-MAC (REG_MACID): the WMAC auto-ACKs a received unicast ONLY when
     * the frame's RA matches this register (gated by RCR APM, set in macinit). It
     * powers up zero, so without this the AP never hears an ACK and retransmits
     * every data frame -> the ~90% retry storm + rate-control collapse to mcs0. */
    hw_write32(REG_MACID, rtwdev->efuse.addr[0] | ((u32)rtwdev->efuse.addr[1] << 8) |
                          ((u32)rtwdev->efuse.addr[2] << 16) | ((u32)rtwdev->efuse.addr[3] << 24));
    hw_write16(REG_MACID + 4, rtwdev->efuse.addr[4] | ((u32)rtwdev->efuse.addr[5] << 8));

    /* hardware BSSID filter: keep frames from our AP */
    hw_write32(REG_BSSID, bssid[0] | ((u32)bssid[1] << 8) |
                          ((u32)bssid[2] << 16) | ((u32)bssid[3] << 24));
    hw_write16(REG_BSSID + 4, bssid[4] | ((u32)bssid[5] << 8));

    /* put the MAC into infrastructure-STA network type (REG_MSR port-0 = MGD_LINKED).
     * The firmware media-status H2C below is supposed to set this, but we'd never set
     * it in hardware ourselves. Without INFRA, the WMAC doesn't fully act as an
     * associated STA — its auto-ACK/Block-Ack response to the AP's DOWNLINK frames is
     * degraded, so the AP rate-floors us + retransmits (download stalls while upload,
     * which only needs the AP to ACK us, stays fine). Read-modify-write to preserve
     * the other ports' nettype bits. */
    {
        u8 msr = hw_read8(REG_MSR);
        hw_write8(REG_MSR, (u8)((msr & ~0x03) | MSR_NETTYPE_INFRA));
    }

    /* media-status-report: mark the peer MACID connected (firmware runs auto-ACK
     * + rate adaptation for it). word0: [7:0]=cmd, [8]=op_mode(connect), [23:16]=macid */
    u8 h2c[8] = {0};
    h2c[0] = H2C_CMD_MEDIA_STATUS_RPT;
    h2c[1] = 0x01;                 /* BIT(8): op_mode = connect */
    h2c[2] = mac_id;               /* [23:16] */
    h2c_command(rtwdev, h2c);
    usleep_range(5000, 5000);

    /* Enable firmware rate adaptation for this peer, matched to what the AP granted
     * in the association response (g_session). The firmware then climbs/falls within
     * this rate mask per link quality. Bit layout (verified vs. upstream main.c):
     *   CCK [3:0]=0xf  OFDM [11:4]=0xff0  HT-MCS0-7 [19:12]=0xff000  VHT-MCS0-9 [29:20]=0x3ff000
     * Bandwidth stays 20MHz here (channel bonding is a later step), so the rates top
     * out at HT MCS7 (~72Mbps SGI) / VHT MCS8 (~86Mbps) on 1 spatial stream. */
    int five = channel > 14;

    /* response-rate set: the rates the WMAC uses to send our ACK/BlockAck/CTS. Left at the
     * power-on default, our Block-Acks for the AP's A-MPDU can go out at a rate the AP
     * mis-decodes -> it treats them as missing, retransmits, then drops MPDUs after its
     * retry limit (the ~50% downlink holes). Program the band's basic set (upstream
     * RRSR_INIT_5G=0x150 / RRSR_INIT_2G=0x15f). */
    hw_write32(REG_RRSR, five ? 0x150 : 0x15f);

    u8  raid, vht_en = 0;
    u32 mask;
    const char *mode;
    if (g_session.has_vht) {               /* 802.11ac (5GHz only) */
        raid = five ? RTW_RATEID_ARFR1_AC_1SS : RTW_RATEID_ARFR2_AC_2G_1SS;
        mask = 0x00000ff0 | 0x003ff000;    /* OFDM | VHT MCS0-9 (1SS) */
        if (!five) mask |= 0x0000000f;     /* + CCK on 2.4GHz */
        vht_en = 1; mode = "VHT/11ac";
    } else if (g_session.has_ht) {         /* 802.11n */
        raid = five ? RTW_RATEID_GN_N1SS : RTW_RATEID_BGN_20M_1SS;
        mask = 0x00000ff0 | 0x000ff000;    /* OFDM | HT MCS0-7 (1SS) */
        if (!five) mask |= 0x0000000f;
        mode = "HT/11n";
    } else {                               /* legacy */
        raid = five ? RTW_RATEID_G : RTW_RATEID_BG;
        mask = 0x00000ff0;
        if (!five) mask |= 0x0000000f;
        mode = "legacy";
    }
    /* SGI on: we advertise Short-GI-20 in the HT cap; lifts HT MCS7 @20MHz 65->72.2 Mbps. */
    fw_ra_info(rtwdev, mac_id, raid, mask, 0 /*20MHz*/, 1 /*SGI*/, vht_en);

    /* publish the chosen rate table id so the TX data paths stamp it into every data
     * descriptor (must match the raid the firmware RA is running for this MACID). */
    g_session.raid = raid;
    g_session.init_rate = DESC_RATE54M;    /* neutral init hint; firmware RA overrides */

    printf("  media-status: MACID %u CONNECTED; self-MAC->0x0610 (HW auto-ACK ON), "
           "BSSID filter set, MSR->INFRA-STA, RA=%s (raid %u mask 0x%07x vht %u)\n",
           mac_id, mode, raid, mask, vht_en);
}
