/*
 * assoc.c — 802.11 authentication + association exchange.
 *
 * Talk to an AP bidirectionally: send an open-system
 * AUTH request and receive the AUTH response, then send an ASSOCIATION request
 * (with SSID + rates + a WPA2-PSK/CCMP RSN IE so encrypted APs accept it) and
 * receive the ASSOCIATION response (status + AID). Open-system auth + assoc is
 * the 802.11-layer join; WPA's 4-way handshake handles the traffic keys.
 *
 * RX is polled directly off the ring (the chip DMAs frames regardless of IRQ).
 */
#ifndef KERNEL
#include <stdio.h>
#endif
#include <string.h>
#include "trx_regs.h"
#include "trx.h"
#include "config.h"

struct wifi_session g_session = { 0, 0, RTW_RATEID_G, DESC_RATE54M };

void set_channel(struct rtw_dev *rtwdev, u8 channel, u8 bw, u8 primary_ch_idx);
int  wpa_handshake(struct rtw_dev *rtwdev, const u8 *bssid,
                         const char *ssid, const char *pass, u8 channel);

/* ---- frame builders ----------------------------------------------------- */
static u32 put_hdr(u8 *p, u8 subtype, const u8 *da, const u8 *sa, const u8 *bssid)
{
    p[0] = (u8)(subtype << 4);   /* type=mgmt(0) | subtype */
    p[1] = 0x00;
    p[2] = 0; p[3] = 0;          /* duration */
    memcpy(p + 4, da, 6);
    memcpy(p + 10, sa, 6);
    memcpy(p + 16, bssid, 6);
    p[22] = 0; p[23] = 0;        /* seq ctrl (HW assigns) */
    return 24;
}

static u32 build_auth_req(u8 *buf, const u8 *sa, const u8 *bssid)
{
    u32 n = put_hdr(buf, 11 /*auth*/, bssid, sa, bssid);
    buf[n++] = 0; buf[n++] = 0;  /* auth algorithm = open system */
    buf[n++] = 1; buf[n++] = 0;  /* auth transaction seq = 1 */
    buf[n++] = 0; buf[n++] = 0;  /* status = 0 */
    return n;
}

/* Our baseband only runs 20 MHz (channel.c has no
 * WIDTH_80 case). Advertising VHT makes an 802.11ac AP send us 80 MHz PPDUs we
 * physically can't decode — they vanish as ~40% "miss" with err=0, and the AP
 * rate-floors us to VHT-MCS0 + retransmits (the retry storm). Drop the VHT IE so
 * the AP is forced to 20 MHz HT frames we fully receive. Flip back to 1 ONLY once
 * the 80 MHz baseband path is ported. */
/* 0 = don't advertise VHT. With the ASPM/SIG-B fix RX now flows, and the health log
 * shows ~36% of the AP's SNs simply never arrive (err=0, ringFull=0) — the signature of
 * the AP sending 40/80MHz VHT PPDUs our 20MHz-only baseband can't demodulate. Advertising
 * VHT implies >=80MHz; dropping it (HT cap is 20MHz-only) forces the AP to 20MHz HT we can
 * fully receive. Flip back to 1 only once the 40/80MHz baseband path (set_channel_bb) lands. */
#define RTW_ADVERTISE_VHT 0

static u32 build_assoc_req(u8 *buf, const u8 *sa, const u8 *bssid,
                           const char *ssid, u8 ssid_len, int five_ghz)
{
    u32 n = put_hdr(buf, 0 /*assoc-req*/, bssid, sa, bssid);
    buf[n++] = 0x11; buf[n++] = 0x04;   /* cap: ESS | Privacy | ShortSlotTime */
    buf[n++] = 0x0a; buf[n++] = 0x00;   /* listen interval = 10 */

    buf[n++] = 0; buf[n++] = ssid_len;  /* SSID IE */
    memcpy(buf + n, ssid, ssid_len); n += ssid_len;

    buf[n++] = 1;                       /* supported rates IE */
    if (five_ghz) {
        buf[n++] = 8;
        buf[n++] = 0x8c; buf[n++] = 0x12; buf[n++] = 0x98; buf[n++] = 0x24; /* 6*,9,12*,18 */
        buf[n++] = 0xb0; buf[n++] = 0x48; buf[n++] = 0x60; buf[n++] = 0x6c; /* 24*,36,48,54 */
    } else {
        buf[n++] = 8;
        buf[n++] = 0x82; buf[n++] = 0x84; buf[n++] = 0x8b; buf[n++] = 0x96; /* 1*,2*,5.5*,11* */
        buf[n++] = 0x0c; buf[n++] = 0x12; buf[n++] = 0x18; buf[n++] = 0x24; /* 6,9,12,18 */
    }

    /* RSN IE: WPA2-PSK / CCMP (so WPA2 APs accept the assoc) */
    buf[n++] = 48; buf[n++] = 20;
    buf[n++] = 0x01; buf[n++] = 0x00;                               /* version */
    buf[n++] = 0x00; buf[n++] = 0x0f; buf[n++] = 0xac; buf[n++] = 0x04; /* group CCMP */
    buf[n++] = 0x01; buf[n++] = 0x00;                               /* pairwise cnt */
    buf[n++] = 0x00; buf[n++] = 0x0f; buf[n++] = 0xac; buf[n++] = 0x04; /* pairwise CCMP */
    buf[n++] = 0x01; buf[n++] = 0x00;                               /* akm cnt */
    buf[n++] = 0x00; buf[n++] = 0x0f; buf[n++] = 0xac; buf[n++] = 0x02; /* akm PSK */
    buf[n++] = 0x00; buf[n++] = 0x00;                               /* rsn cap */

    /* HT Capabilities (tag 45, 26B) — advertise that we can receive 802.11n so the
     * AP rate-controls us with HT MCS instead of legacy OFDM. 1 spatial stream,
     * 20MHz (we don't set the 20/40 width bit yet — channel bonding is a later
     * step), Short-GI-20 + A-MSDU. The MCS map (rx_mask[0]=0xff) = MCS0-7. */
    buf[n++] = 45; buf[n++] = 26;
    buf[n++] = 0x20; buf[n++] = 0x08;   /* HT cap info: SGI-20 | Max-A-MSDU (the kext  */
                                        /* now de-aggregates A-MSDU, so invite it)     */
    buf[n++] = 0x0b;                    /* A-MPDU params: 64K factor, density 2    */
    buf[n++] = 0xff;                    /* Supported MCS set: RX MCS0-7 (1SS)      */
    for (int i = 0; i < 15; i++) buf[n++] = 0x00;  /* rest of 16B MCS set          */
    buf[n++] = 0x00; buf[n++] = 0x00;   /* HT extended capabilities                */
    buf[n++] = 0x00; buf[n++] = 0x00; buf[n++] = 0x00; buf[n++] = 0x00; /* TxBF    */
    buf[n++] = 0x00;                    /* ASEL capabilities                       */

    /* VHT Capabilities (tag 191, 12B) — 5GHz only (VHT is undefined in 2.4GHz).
     * Lets an 802.11ac AP send us VHT MCS. 1SS, MCS0-9 (rx/tx map 0xfffe). */
    if (five_ghz && RTW_ADVERTISE_VHT) {
        buf[n++] = 191; buf[n++] = 12;
        buf[n++] = 0x02; buf[n++] = 0x00; buf[n++] = 0x00; buf[n++] = 0x00; /* cap: max MPDU 11454 */
        buf[n++] = 0xfe; buf[n++] = 0xff; /* rx_mcs_map = 0xfffe (SS1=MCS0-9)       */
        buf[n++] = 0x00; buf[n++] = 0x00; /* rx highest rate                        */
        buf[n++] = 0xfe; buf[n++] = 0xff; /* tx_mcs_map = 0xfffe                     */
        buf[n++] = 0x00; buf[n++] = 0x00; /* tx highest rate                        */
    }
    return n;
}

/* ---- poll the RX ring for a mgmt response from `bssid` of subtype `want` -- */
static int wait_mgmt_resp(struct rtw_dev *rtwdev, const u8 *bssid, u8 want,
                          u8 *body, u32 *body_len, u32 ms)
{
    u32 n = trx_rx_count();
    u32 rp = trx_rx_hw_idx();          /* skip stale frames */
    trx_rx_set_host_idx(rtwdev, rp);
    for (u32 t = 0; t < ms; t += 5) {
        usleep_range(5000, 5000);
        u32 hw = trx_rx_hw_idx();
        while (rp != hw) {
            u8 *buf = trx_rx_slot_buf(rp);
            if (buf) {
                u32 w0 = *(u32 *)buf;
                u32 pkt_len = w0 & 0x3fff, drv = (w0 >> 16) & 0xf, shift = (w0 >> 24) & 0x3;
                if (pkt_len >= 28 && pkt_len <= 2048) {
                    u8 *f = buf + 24 + shift + drv * 8;
                    u16 fc = (u16)(f[0] | (f[1] << 8));
                    u8 type = (fc >> 2) & 0x3, sub = (fc >> 4) & 0xf;
                    if (type == 0 && sub == want && memcmp(f + 16, bssid, 6) == 0) {
                        u32 blen = pkt_len - 24;
                        if (blen > *body_len) blen = *body_len;
                        memcpy(body, f + 24, blen);
                        *body_len = blen;
                        rp = (rp + 1) % n;
                        trx_rx_set_host_idx(rtwdev, rp);
                        return 1;
                    }
                }
            }
            rp = (rp + 1) % n;
        }
        trx_rx_set_host_idx(rtwdev, rp);
    }
    return 0;
}

/* ---- after assoc: listen for the AP's EAPOL (4-way handshake start) ------ *
 * Promiscuous RX catches the AP's data frames to us. An EAPOL-Key frame (LLC
 * SNAP EtherType 0x888E) is Message 1 of the WPA2 4-way handshake — proof the AP
 * treats us as a connected client. Responding needs the passphrase. */
static void listen_eapol(struct rtw_dev *rtwdev, const u8 *bssid)
{
    u32 n = trx_rx_count();
    u32 rp = trx_rx_hw_idx();
    trx_rx_set_host_idx(rtwdev, rp);
    int data = 0, eapol = 0;
    printf("  listening ~2s for AP data / EAPOL (4-way handshake)...\n");
    for (u32 t = 0; t < 2000 && !eapol; t += 5) {
        usleep_range(5000, 5000);
        u32 hw = trx_rx_hw_idx();
        while (rp != hw) {
            u8 *buf = trx_rx_slot_buf(rp);
            if (buf) {
                u32 w0 = *(u32 *)buf;
                u32 pkt_len = w0 & 0x3fff, drv = (w0 >> 16) & 0xf, shift = (w0 >> 24) & 0x3;
                if (pkt_len >= 32 && pkt_len <= 2048) {
                    u8 *f = buf + 24 + shift + drv * 8;
                    u16 fc = (u16)(f[0] | (f[1] << 8));
                    u8 type = (fc >> 2) & 0x3, sub = (fc >> 4) & 0xf;
                    if (type == 2 && memcmp(f + 10, bssid, 6) == 0) {   /* data from AP */
                        data++;
                        u32 hdr = 24 + ((sub & 0x8) ? 2 : 0);           /* QoS-data: +2 */
                        u8 *llc = f + hdr;
                        if (pkt_len >= hdr + 8 &&
                            llc[0] == 0xaa && llc[1] == 0xaa && llc[6] == 0x88 && llc[7] == 0x8e) {
                            eapol++;
                            printf("  EAPOL-Key frame from AP \xE2\x9C\x93  (WPA2 4-way handshake started) len=%u\n",
                                   pkt_len);
                        }
                    }
                }
            }
            rp = (rp + 1) % n;
        }
        trx_rx_set_host_idx(rtwdev, rp);
    }
    printf("  post-assoc: %d data frames, %d EAPOL frames from the AP\n", data, eapol);
    if (eapol)
        printf("  => the AP is running the 4-way handshake with us. The passphrase is\n"
               "     needed to compute the PTK/MIC to complete it and pass traffic.\n");
}

/* ---- the join: auth then assoc ------------------------------------------ */
int associate(struct rtw_dev *rtwdev, const u8 *bssid,
                    const char *ssid, u8 ssid_len, u8 channel, const char *pass)
{
    int five = channel > 14;
    u8 rate = five ? DESC_RATE6M : DESC_RATE1M;
    u8 frame[256], body[160];
    u32 flen, blen;

    printf("\n=== associate to \"%s\" %02x:%02x:%02x:%02x:%02x:%02x ch %u ===\n",
           ssid, bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], channel);
    set_channel(rtwdev, channel, RTW_CHANNEL_WIDTH_20, 0);

    /* AUTH (open system) */
    int auth_ok = 0;
    for (int t = 0; t < 6 && !auth_ok; t++) {
        flen = build_auth_req(frame, rtwdev->efuse.addr, bssid);
        trx_tx_mgmt(rtwdev, frame, flen, rate, 0);
        blen = sizeof(body);
        if (wait_mgmt_resp(rtwdev, bssid, 11, body, &blen, 250)) {
            u16 seq = (u16)(body[2] | (body[3] << 8));
            u16 status = (u16)(body[4] | (body[5] << 8));
            printf("  AUTH response: seq=%u status=%u (%s)\n",
                   seq, status, status == 0 ? "OK" : "rejected");
            if (status == 0) auth_ok = 1;
            else break;
        }
    }
    if (!auth_ok) { printf("  RESULT: no/failed AUTH response\n"); return -1; }

    /* ASSOCIATION */
    for (int t = 0; t < 6; t++) {
        flen = build_assoc_req(frame, rtwdev->efuse.addr, bssid, ssid, ssid_len, five);
        trx_tx_mgmt(rtwdev, frame, flen, rate, 0);
        blen = sizeof(body);
        if (wait_mgmt_resp(rtwdev, bssid, 1, body, &blen, 250)) {
            u16 status = (u16)(body[2] | (body[3] << 8));
            u16 aid = (u16)(body[4] | (body[5] << 8)) & 0x3fff;
            printf("  ASSOC response: status=%u aid=%u (%s)\n",
                   status, aid, status == 0 ? "ASSOCIATED" : "rejected");
            if (status == 0) {
                /* learn what the AP granted: walk the response IEs (body[6..] after
                 * cap/status/aid) for HT(45)/VHT(191) so media-connect programs the
                 * matching rate table. Authoritative vs. guessing from our request. */
                g_session.has_ht = g_session.has_vht = 0;
                for (u32 i = 6; i + 2 <= blen; ) {
                    u8 t = body[i], l = body[i + 1];
                    if (i + 2 + l > blen) break;
                    if (t == 45)       g_session.has_ht = 1;
                    else if (t == 191) g_session.has_vht = 1;
                    i += 2 + l;
                }
                printf("  RESULT: ASSOCIATED \xE2\x9C\x93  (802.11 join complete, AID %u; AP caps:%s%s%s)\n",
                       aid, g_session.has_vht ? " VHT" : "", g_session.has_ht ? " HT" : "",
                       (!g_session.has_ht && !g_session.has_vht) ? " legacy" : "");
                if (pass && pass[0])
                    wpa_handshake(rtwdev, bssid, ssid, pass, channel);
                else
                    listen_eapol(rtwdev, bssid);
                return 0;
            }
            return -2;
        }
    }
    printf("  RESULT: AUTH ok but no ASSOC response\n");
    return -3;
}
