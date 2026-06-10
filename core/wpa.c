/*
 * wpa.c — WPA2-PSK 4-way handshake (RSN / CCMP).
 *
 *   PMK = PBKDF2-HMAC-SHA1(passphrase, SSID, 4096, 32)          [crypto/rtw_crypto]
 *   M1 (AP->STA): ANonce
 *   PTK = PRF-384(PMK, "Pairwise key expansion",
 *                 min(AA,SPA)|max|min(ANonce,SNonce)|max)        -> KCK|KEK|TK
 *   M2 (STA->AP): SNonce + RSN IE + MIC(KCK)
 *   M3 (AP->STA): ANonce + GTK(enc) + MIC  -> verify MIC == correct PSK/PTK
 *   M4 (STA->AP): MIC
 *
 * EAPOL rides on 802.11 DATA frames (trx_tx_data); RX is polled off the ring.
 * Multi-byte EAPOL fields are big-endian. Key descriptor version 2 (AES): MIC =
 * first 16 bytes of HMAC-SHA1(KCK, EAPOL-with-MIC-zeroed).
 */
#ifndef KERNEL
#include <stdio.h>
#include <stdlib.h>
#endif
#include <string.h>
#include "trx_regs.h"
#include "trx.h"
#include "../crypto/rtw_crypto.h"   /* bundled SHA1/HMAC/PBKDF2/AES — no CommonCrypto */

void set_channel(struct rtw_dev *rtwdev, u8 channel, u8 bw, u8 primary_ch_idx);
void install_keys(struct rtw_dev *rtwdev, const u8 *bssid);
void media_connect(struct rtw_dev *rtwdev, const u8 *bssid, u8 mac_id, u8 channel);

/* the derived keys (exposed for key install) */
u8 g_kck[16], g_kek[16], g_tk[16], g_gtk[32];
int g_gtk_len, g_gtk_keyidx, g_wpa_done;

static void put_be16(u8 *p, u16 v) { p[0] = v >> 8; p[1] = v & 0xff; }
static u16  get_be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }

/* PRF-n (HMAC-SHA1) per 802.11i */
static void prf(const u8 *key, int keylen, const char *label,
                const u8 *seed, int seedlen, u8 *out, int outlen)
{
    int labellen = (int)strlen(label);
    u8 in[128], digest[20];
    int pos = 0, i = 0;
    while (pos < outlen) {
        int n = 0;
        memcpy(in + n, label, labellen); n += labellen;
        in[n++] = 0x00;
        memcpy(in + n, seed, seedlen); n += seedlen;
        in[n++] = (u8)i;
        rtw_hmac_sha1((const uint8_t *)key, (size_t)keylen, in, (size_t)n, digest);
        int c = (outlen - pos < 20) ? outlen - pos : 20;
        memcpy(out + pos, digest, c);
        pos += c; i++;
    }
}

static void derive_ptk(const u8 *pmk, const u8 *aa, const u8 *spa,
                       const u8 *anonce, const u8 *snonce, u8 *ptk48)
{
    u8 seed[76];
    if (memcmp(aa, spa, 6) < 0) { memcpy(seed, aa, 6); memcpy(seed + 6, spa, 6); }
    else                        { memcpy(seed, spa, 6); memcpy(seed + 6, aa, 6); }
    if (memcmp(anonce, snonce, 32) < 0) { memcpy(seed + 12, anonce, 32); memcpy(seed + 44, snonce, 32); }
    else                                { memcpy(seed + 12, snonce, 32); memcpy(seed + 44, anonce, 32); }
    prf(pmk, 32, "Pairwise key expansion", seed, 76, ptk48, 48);
}

/* AES-ECB single-block decrypt, for RFC 3394 key unwrap */
static void aes_dec_block(const u8 *kek, const u8 *in, u8 *out)
{
    rtw_aes128_ecb_decrypt(kek, in, out);
}

/* RFC 3394 AES key unwrap. clen = ciphertext bytes (n+1 64-bit blocks).
 * On success writes n*8 plaintext bytes to out and returns n*8, else 0. */
static int aes_unwrap(const u8 *kek, const u8 *cipher, int clen, u8 *out)
{
    int n = clen / 8 - 1;
    if (n < 1 || clen % 8) return 0;
    u8 a[8], r[32 * 8], b[16];
    memcpy(a, cipher, 8);
    memcpy(r, cipher + 8, n * 8);
    for (int j = 5; j >= 0; j--) {
        for (int i = n; i >= 1; i--) {
            u64 t = (u64)(n * j + i);
            memcpy(b, a, 8);
            for (int k = 0; k < 8; k++) b[7 - k] ^= (u8)(t >> (8 * k));
            memcpy(b + 8, r + (i - 1) * 8, 8);
            aes_dec_block(kek, b, b);
            memcpy(a, b, 8);
            memcpy(r + (i - 1) * 8, b + 8, 8);
        }
    }
    for (int k = 0; k < 8; k++) if (a[k] != 0xa6) return 0;  /* default IV check */
    memcpy(out, r, n * 8);
    return n * 8;
}

/* parse unwrapped EAPOL Key Data for a GTK KDE (00-0F-AC type 1). The unwrapped
 * key data is attacker-influenced (any party knowing the PSK can wrap valid data),
 * so every TLV access is bounds-checked against `len` before it is read. */
static int parse_gtk_kde(const u8 *kd, int len, u8 *gtk, int *keyidx)
{
    static const u8 oui[3] = { 0x00, 0x0f, 0xac };
    int i = 0;
    while (i + 2 <= len) {
        u8 type = kd[i], dlen = kd[i + 1];
        if (i + 2 + dlen > len) break;    /* truncated TLV — stop, don't read past len */
        if (type == 0xdd && dlen >= 6 &&
            memcmp(kd + i + 2, oui, 3) == 0 && kd[i + 5] == 0x01) {
            /* GTK KDE: [keyid(2b)+tx][rsvd][GTK...] */
            *keyidx = kd[i + 6] & 0x03;
            int glen = dlen - 6;          /* OUI(3)+datatype(1)+keyinfo(2) */
            if (glen > 32) glen = 32;
            memcpy(gtk, kd + i + 8, glen);
            return glen;
        }
        if (type == 0) break;
        i += 2 + dlen;
    }
    return 0;
}

/* extract the GTK from an M3 EAPOL-Key frame (key data AES-unwrapped with KEK).
 * eapol points at the EAPOL header (elen received bytes); the descriptor body
 * starts at eapol+4. Returns 1 if a usable GTK landed in g_gtk. */
static int extract_gtk(const u8 *eapol, u32 elen)
{
    const u8 *kd = eapol + 4;
    int data_len = get_be16(kd + 93);     /* key data length (frame-supplied) */
    if (data_len < 16 || data_len > 256 || (data_len % 8)) return 0;
    if (4u + 95u + (u32)data_len > elen) return 0;   /* claimed length beyond received bytes */
    u8 plain[256];
    int plen = aes_unwrap(g_kek, kd + 95, data_len, plain);
    if (!plen) { printf("  GTK unwrap failed\n"); return 0; }
    int glen = parse_gtk_kde(plain, plen, g_gtk, &g_gtk_keyidx);
    if (glen >= 16) {
        g_gtk_len = glen;
        printf("  GTK extracted (%d bytes, key id %d) \xE2\x9C\x93\n", glen, g_gtk_keyidx);
        return 1;
    }
    return 0;
}

/* build the 802.11 DATA header + LLC SNAP for an EAPOL frame to the AP */
static u32 eapol_hdr(u8 *p, const u8 *bssid, const u8 *sa)
{
    p[0] = 0x08; p[1] = 0x01;          /* data, ToDS */
    p[2] = 0; p[3] = 0;
    memcpy(p + 4, bssid, 6);           /* addr1 = BSSID (to AP) */
    memcpy(p + 10, sa, 6);             /* addr2 = SA (us) */
    memcpy(p + 16, bssid, 6);          /* addr3 = DA (AP) */
    p[22] = 0; p[23] = 0;
    static const u8 snap[8] = { 0xaa, 0xaa, 0x03, 0, 0, 0, 0x88, 0x8e };
    memcpy(p + 24, snap, 8);
    return 32;                         /* 24 hdr + 8 SNAP */
}

/* the WPA2-PSK/CCMP RSN IE we advertise (matches the assoc request) */
static const u8 rsn_ie[22] = {
    48, 20, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00,
    0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00
};

/* poll RX for the next EAPOL-Key frame from the AP; copies the EAPOL portion
 * (header + key body) to `out`. Returns its length, or 0 on timeout. */
static u32 recv_eapol(struct rtw_dev *rtwdev, const u8 *bssid, u32 *rp_io,
                      u8 *out, u32 outcap, u32 ms)
{
    u32 n = trx_rx_count();
    u32 rp = *rp_io;            /* resume where the previous call stopped — do NOT snap to the
                                 * live hw index, or a frame DMA'd between calls (e.g. M3
                                 * landing while we rebuild/resend M2) is skipped forever. */
    for (u32 t = 0; t < ms; t += 5) {
        usleep_range(5000, 5000);
        u32 hw = trx_rx_hw_idx();
        while (rp != hw) {
            u8 *buf = trx_rx_slot_buf(rp);
            if (buf) {
                u32 w0 = *(u32 *)buf;
                u32 pkt_len = w0 & 0x3fff, drv = (w0 >> 16) & 0xf, shift = (w0 >> 24) & 0x3;
                if ((w0 & (BIT(14) | BIT(15))) == 0 &&   /* skip HW-flagged CRC32/ICV-error frames */
                    pkt_len >= 32 && pkt_len <= 2048) {
                    u8 *f = buf + 24 + shift + drv * 8;
                    u16 fc = (u16)(f[0] | (f[1] << 8));
                    u8 type = (fc >> 2) & 0x3, sub = (fc >> 4) & 0xf;
                    if (type == 2 && memcmp(f + 10, bssid, 6) == 0) {
                        /* data hdr = 24 + QoS(2 if subtype bit3) + HT-Control(4 if QoS AND
                         * Order/+HTC, fc bit15 — in non-QoS data the Order bit means
                         * "strictly ordered", no HTC field) — without the +HTC term an
                         * EAPOL in a QoS+HTC frame is mis-located and dropped. */
                        u32 qos = (sub & 0x8) ? 2 : 0;
                        u32 hdr = 24 + qos + ((qos && (fc & 0x8000)) ? 4 : 0);
                        u8 *llc = f + hdr;
                        if (pkt_len >= hdr + 8 + 4 &&
                            llc[0] == 0xaa && llc[6] == 0x88 && llc[7] == 0x8e) {
                            u8 *eapol = llc + 8;
                            u32 avail = pkt_len - hdr - 8;        /* EAPOL bytes actually received */
                            u32 elen = 4 + get_be16(eapol + 2);   /* hdr + claimed body */
                            if (elen > outcap) elen = outcap;
                            if (elen > avail)  elen = avail;      /* clamp to RECEIVED bytes, not pkt_len
                                                                   * (pkt_len includes the 802.11 header,
                                                                   * so the old clamp could copy past the
                                                                   * MPDU into stale slot bytes) */
                            /* a 4-way EAPOL-Key frame is >= 99 bytes (4 hdr + 95 body); the
                             * parsers index up to the MIC at 81..96 — skip short frames */
                            if (elen >= 99) {
                                memcpy(out, eapol, elen);
                                rp = (rp + 1) % n;
                                trx_rx_set_host_idx(rtwdev, rp);
                                *rp_io = rp;
                                return elen;
                            }
                        }
                    }
                }
            }
            rp = (rp + 1) % n;
        }
        trx_rx_set_host_idx(rtwdev, rp);
    }
    *rp_io = rp;
    return 0;
}

/* constant-time 16-byte compare (MIC check): a byte-wise early-exit memcmp leaks
 * how many MIC bytes matched through timing. Returns 0 when equal. */
static int ct_memcmp16(const u8 *a, const u8 *b)
{
    u8 d = 0;
    for (int i = 0; i < 16; i++) d |= (u8)(a[i] ^ b[i]);
    return d;
}

/* best-effort secure wipe (memset the compiler must not elide) */
static void wipe(void *p, size_t n)
{
    volatile u8 *v = (volatile u8 *)p;
    while (n--) *v++ = 0;
}

/* compute MIC (HMAC-SHA1, first 16 bytes) over an EAPOL frame with MIC zeroed */
static void eapol_mic(const u8 *kck, u8 *eapol, u32 elen, u8 *mic_out)
{
    u8 save[16];
    u8 *mic = eapol + 81;   /* 4 hdr + (1+2+2+8+32+16+8+8) = 81 -> MIC field */
    memcpy(save, mic, 16);
    memset(mic, 0, 16);
    u8 digest[20];
    rtw_hmac_sha1(kck, 16, eapol, (size_t)elen, digest);
    memcpy(mic_out, digest, 16);
    memcpy(mic, save, 16);
}

/* build M2 or M4. key_info per message; m2 carries SNonce+RSN IE. */
static u32 build_eapol_key(u8 *eapol, u16 key_info, const u8 *replay,
                           const u8 *snonce, const u8 *kck, int include_rsn)
{
    u32 kd_len = include_rsn ? sizeof(rsn_ie) : 0;
    u16 body_len = (u16)(95 + kd_len);
    u8 *b = eapol + 4;                 /* key body */
    eapol[0] = 0x02; eapol[1] = 0x03;  /* EAPOL ver 2, type Key */
    put_be16(eapol + 2, body_len);
    memset(b, 0, 95);
    b[0] = 0x02;                       /* descriptor type RSN */
    put_be16(b + 1, key_info);
    put_be16(b + 3, 16);               /* key length (CCMP) */
    memcpy(b + 5, replay, 8);
    if (snonce) memcpy(b + 13, snonce, 32);
    /* iv/rsc/reserved already zero; MIC (b+77..92) zero for now */
    put_be16(b + 93, (u16)kd_len);
    if (include_rsn) memcpy(b + 95, rsn_ie, kd_len);
    u32 elen = 4 + body_len;
    u8 mic[16];
    eapol_mic(kck, eapol, elen, mic);
    memcpy(b + 77, mic, 16);
    return elen;
}

int wpa_handshake(struct rtw_dev *rtwdev, const u8 *bssid,
                        const char *ssid, const char *pass, u8 channel)
{
    u8 pmk[32], ptk[48], snonce[32], anonce[32], replay[8];
    u8 eapol[512], frame[600];
    u8 rate = channel > 14 ? DESC_RATE6M : DESC_RATE1M;
    const u8 *sa = rtwdev->efuse.addr;
    int rc = 0;

    /* reset the GTK globals: extract_gtk only writes them on success, so without this
     * a reconnect whose M3 lacks a usable GTK would install the PREVIOUS network's
     * group key into the CAM (stale-key install, undetected) */
    g_gtk_len = 0; g_gtk_keyidx = 0;
    memset(g_gtk, 0, sizeof(g_gtk));

    printf("\n=== WPA2 4-way handshake (\"%s\") ===\n", ssid);
    rtw_pbkdf2_sha1((const uint8_t *)pass, strlen(pass),
                    (const uint8_t *)ssid, strlen(ssid), 4096, pmk, 32);
    arc4random_buf(snonce, 32);

    /* --- M1: wait for the AP's first EAPOL-Key (ANonce) --- */
    u32 rp = trx_rx_hw_idx();      /* snap the RX read pointer ONCE; recv_eapol advances it
                                    * across calls so no EAPOL frame is skipped mid-handshake */
    trx_rx_set_host_idx(rtwdev, rp);
    u32 elen = recv_eapol(rtwdev, bssid, &rp, eapol, sizeof(eapol), 2000);
    if (!elen) { printf("  no EAPOL M1 received\n"); rc = -2; goto out; }
    memcpy(replay, eapol + 4 + 5, 8);
    memcpy(anonce, eapol + 4 + 13, 32);
    printf("  M1 received (ANonce, replay %02x..%02x)\n", replay[0], replay[7]);

    derive_ptk(pmk, bssid, sa, anonce, snonce, ptk);
    memcpy(g_kck, ptk, 16); memcpy(g_kek, ptk + 16, 16); memcpy(g_tk, ptk + 32, 16);

    /* --- M2: SNonce + RSN IE + MIC --- */
    u32 hl = eapol_hdr(frame, bssid, sa);
    u32 m2 = build_eapol_key(frame + hl, 0x010a /*ver2|pairwise|MIC*/, replay, snonce, g_kck, 1);
    trx_tx_data(rtwdev, frame, hl + m2, rate);
    printf("  M2 sent (SNonce + MIC)\n");

    /* --- M3: AP replies if our MIC/PSK is correct --- */
    /* Budget genuine SILENCES, not received frames: an M1-retransmit burst (the AP retries
     * M1 until it hears our M2) must not exhaust the retry count, or we give up before M3
     * arrives — the "no valid M3" symptom. So a received M1 does NOT consume a timeout, and
     * we rebuild M2 with THAT M1's replay counter (802.11-2020 12.7.6.3: M2 must echo the
     * counter of the M1 it answers; APs that bump the EAPOL replay counter on retransmit
     * reject a stale M2). 8 real 500ms silences ~= 4s. */
    /* Bound BOTH genuine silences AND total iterations: a flood of M1-retransmits or
     * MIC-failing frames must not spin this loop (it runs in-kernel under the control
     * lock). 8 real 500ms silences ~= 4s; the iteration cap is a hard ceiling (~30
     * received frames) so a malicious EAPOL flood can't hold the loop open. */
    int ok = 0, timeouts = 0, iters = 0;
    while (!ok && timeouts < 8 && iters < 30) {
        iters++;
        elen = recv_eapol(rtwdev, bssid, &rp, eapol, sizeof(eapol), 500);
        if (!elen) { /* genuine silence -> retransmit M2 and spend one of the budget */
            trx_tx_data(rtwdev, frame, hl + m2, rate);
            timeouts++;
            continue;
        }
        u16 ki = get_be16(eapol + 4 + 1);
        if (!(ki & 0x0100)) {                    /* not a MIC frame: an M1 retransmit */
            memcpy(replay, eapol + 4 + 5, 8);    /* answer THIS M1's replay counter */
            m2 = build_eapol_key(frame + hl, 0x010a, replay, snonce, g_kck, 1);
            trx_tx_data(rtwdev, frame, hl + m2, rate);
            continue;                            /* M1 frames don't consume the budget */
        }
        /* verify M3 MIC with our KCK (constant-time). The MIC over KCK — derived from the
         * PMK — is the authentication: only the real AP can produce it, so we do NOT also
         * gate on the replay counter relative to the (unauthenticated, forgeable) M1, which
         * could let a forged high-counter M1 make us reject the genuine M3. */
        u8 want[16]; memcpy(want, eapol + 81, 16);
        u8 got[16]; eapol_mic(g_kck, eapol, elen, got);
        if (ct_memcmp16(want, got) != 0) {
            printf("  M3 MIC ignored (forged or stale EAPOL-Key)\n");
            continue;                            /* not from our AP — don't fail the handshake */
        }
        memcpy(replay, eapol + 4 + 5, 8);
        printf("  M3 received, MIC VERIFIED \xE2\x9C\x93  (PTK correct, passphrase OK)\n");

        /* M3 carries the GTK (AES-key-wrapped in the key data). Without it we could
         * not decrypt ANY broadcast/multicast (ARP, DHCP) — that's a broken link that
         * would otherwise be reported as success, so treat it as a handshake failure. */
        if (!extract_gtk(eapol, elen)) {
            printf("  M3 carried no usable GTK — aborting (broadcast RX would be dead)\n");
            rc = -5; goto out;
        }

        /* --- M4: ACK --- */
        u32 m4 = build_eapol_key(frame + hl, 0x030a /*ver2|pairwise|MIC|secure*/, replay, NULL, g_kck, 0);
        trx_tx_data(rtwdev, frame, hl + m4, rate);
        printf("  M4 sent\n");

        /* brief retransmit window: if the AP did not hear M4 it retransmits M3 (same
         * or bumped replay counter). Answer with a fresh M4 instead of leaving the AP
         * to time the handshake out after we already declared success. */
        for (int w = 0; w < 3; w++) {
            u32 rlen = recv_eapol(rtwdev, bssid, &rp, eapol, sizeof(eapol), 150);
            if (!rlen) break;
            u16 rki = get_be16(eapol + 4 + 1);
            if (!(rki & 0x0100) || !(rki & 0x0008)) continue; /* only MIC'd PAIRWISE (M3) frames */
            u8 chk[16]; eapol_mic(g_kck, eapol, rlen, chk);
            if (ct_memcmp16(eapol + 81, chk) != 0) continue; /* not from our AP */
            memcpy(replay, eapol + 4 + 5, 8);
            m4 = build_eapol_key(frame + hl, 0x030a, replay, NULL, g_kck, 0);
            trx_tx_data(rtwdev, frame, hl + m4, rate);
            printf("  M3 retransmit answered with M4\n");
        }
        ok = 1;
    }
    if (!ok) { printf("  no valid M3 (handshake did not complete)\n"); rc = -4; goto out; }

    g_wpa_done = 1;
    printf("  RESULT: 4-WAY HANDSHAKE COMPLETE \xE2\x9C\x93  (TK derived)\n");

    /* install the pairwise TK into the hardware CAM + enable CCMP engine */
    install_keys(rtwdev, bssid);

    /* tell the firmware our peer MACID is connected (auto-ACK). The kext owns the
     * data path separately (kctl.c starts it after connect), so the handshake is
     * complete here. */
    printf("\n=== media-connect ===\n");
    media_connect(rtwdev, bssid, 0, channel);

out:
    /* Zeroize key material. The locals always; the global KCK/KEK/TK/GTK once the
     * hardware CAM holds the session keys (or on failure) — they would otherwise sit
     * in kernel memory for the life of the connection. NOTE: GTK-rekey support (when
     * added) must keep the KEK to unwrap the new GTK — re-derive or retain it then. */
    wipe(pmk, sizeof(pmk)); wipe(ptk, sizeof(ptk));
    wipe(snonce, sizeof(snonce)); wipe(anonce, sizeof(anonce));
    wipe(eapol, sizeof(eapol)); wipe(frame, sizeof(frame));
    wipe(g_kck, sizeof(g_kck)); wipe(g_kek, sizeof(g_kek));
    wipe(g_tk, sizeof(g_tk));   wipe(g_gtk, sizeof(g_gtk));
    return rc;
}
