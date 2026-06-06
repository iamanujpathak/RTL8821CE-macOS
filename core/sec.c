/*
 * sec.c — install the WPA2 keys into the hardware security CAM.
 *
 * After the 4-way handshake, write the pairwise TK (and optionally the GTK) into
 * the chip's security CAM and enable the crypto engine, so the hardware does
 * CCMP: it decrypts received frames and encrypts our TX (Protected bit +
 * tx-desc sec_type=3). Verbatim CAM-write protocol from upstream rtw88 sec.c.
 */
#ifndef KERNEL
#include <stdio.h>
#endif
#include "trx_regs.h"

#define RTW_SEC_CMD_REG          0x670
#define RTW_SEC_WRITE_REG        0x674
#define RTW_SEC_CONFIG           0x680
#define RTW_SEC_CAM_ENTRY_SHIFT  3
#define RTW_SEC_CMD_WRITE_ENABLE BIT(16)
#define RTW_SEC_CMD_POLLING      BIT(31)
#define RTW_SEC_TX_UNI_USE_DK    BIT(0)
#define RTW_SEC_RX_UNI_USE_DK    BIT(1)
#define RTW_SEC_TX_DEC_EN        BIT(2)
#define RTW_SEC_RX_DEC_EN        BIT(3)
#define RTW_SEC_TX_BC_USE_DK     BIT(6)
#define RTW_SEC_RX_BC_USE_DK     BIT(7)
#define RTW_SEC_ENGINE_EN        BIT(9)   /* in REG_CR */
#define RTW_CAM_AES              4

extern u8 g_tk[16], g_gtk[32];
extern int g_gtk_len, g_gtk_keyidx;

/* rtw_sec_write_cam (verbatim): 8 dwords per entry — ctrl, mac, 16-byte key */
static void write_cam(struct rtw_dev *rtwdev, u8 hw_key_idx, u8 keyidx, u8 group,
                      const u8 *mac, const u8 *key)
{
    u32 write_cmd = RTW_SEC_CMD_WRITE_ENABLE | RTW_SEC_CMD_POLLING;
    u32 addr = (u32)hw_key_idx << RTW_SEC_CAM_ENTRY_SHIFT;
    for (int i = 7; i >= 0; i--) {
        u32 content;
        if (i == 0)
            content = (keyidx & 0x3) | ((RTW_CAM_AES & 0x7) << 2) | ((u32)group << 6) |
                      (1u << 15) | ((u32)mac[0] << 16) | ((u32)mac[1] << 24);
        else if (i == 1)
            content = mac[2] | ((u32)mac[3] << 8) | ((u32)mac[4] << 16) | ((u32)mac[5] << 24);
        else if (i >= 6)
            content = 0;
        else {
            int j = (i - 2) << 2;
            content = key[j] | ((u32)key[j+1] << 8) | ((u32)key[j+2] << 16) | ((u32)key[j+3] << 24);
        }
        hw_write32(RTW_SEC_WRITE_REG, content);
        hw_write32(RTW_SEC_CMD_REG, write_cmd | (addr + i));
    }
}

/* default_key_search mode (upstream rtw_sec_enable_sec_engine): CAM entries 0-3
 * are reserved for GROUP keys, direct-mapped by key index; PAIRWISE keys MUST be
 * installed at entries >= RTW_SEC_DEFAULT_KEY_NUM (4) where they are found by the
 * peer's MAC address. Installing the pairwise key at entry 0 silently collides
 * with the group-key region and unicast CCMP fails (bad TX MIC / RX undecrypted). */
#define RTW_SEC_DEFAULT_KEY_NUM  4

void install_keys(struct rtw_dev *rtwdev, const u8 *bssid)
{
    static const u8 bcast[6] = { 0xff,0xff,0xff,0xff,0xff,0xff };

    /* pairwise TK at CAM entry 4 (group=0, keyidx 0, addressed to the AP) */
    write_cam(rtwdev, RTW_SEC_DEFAULT_KEY_NUM, 0, 0, bssid, g_tk);
    /* group GTK at CAM entry == its key index (0-3, the direct-map region) */
    if (g_gtk_len >= 16)
        write_cam(rtwdev, g_gtk_keyidx, g_gtk_keyidx, 1, bcast, g_gtk);

    /* enable the crypto engine + default-key search for uni/bc TX & RX */
    u16 cr = hw_read16(REG_CR);
    hw_write16(REG_CR, cr | RTW_SEC_ENGINE_EN);
    u16 sc = hw_read16(RTW_SEC_CONFIG);
    sc |= RTW_SEC_TX_DEC_EN | RTW_SEC_RX_DEC_EN |
          RTW_SEC_TX_UNI_USE_DK | RTW_SEC_RX_UNI_USE_DK |
          RTW_SEC_TX_BC_USE_DK | RTW_SEC_RX_BC_USE_DK;
    hw_write16(RTW_SEC_CONFIG, sc);

    printf("  keys installed: pairwise TK (CAM%d)%s; CCMP engine ON\n",
           RTW_SEC_DEFAULT_KEY_NUM, g_gtk_len >= 16 ? " + GTK (CAM idx=keyid)" : "");
}
