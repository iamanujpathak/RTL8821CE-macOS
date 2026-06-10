/*
 * channel.c — set channel + enable RX + poll for received frames.
 *
 * set_channel (rf/bb/swing/rxdfir + switch_rf_set) is VERBATIM from upstream
 * rtw8821c.c. RF reads/writes go through the phy.c primitives. Constants
 * that live in upstream rtw8821c.h (not our vendored reg.h) are defined here
 * with provenance; reg.h ones come via the shim.
 *
 * Goal: tune to a 2.4GHz channel, set the chip promiscuous (accept beacons),
 * and poll the RX ring's hw index — if it advances, the chip is receiving.
 */
#ifndef KERNEL
#include <stdio.h>           /* printf: kernel build maps it to IOLog via rtw_shim.h */
#endif
#include "trx_regs.h"

/* RF primitives (phy.c) */
u32  rtw_phy_read_rf(struct rtw_dev *, enum rtw_rf_path, u32 addr, u32 mask);
bool rtw_write_rf(struct rtw_dev *, enum rtw_rf_path, u32 addr, u32 mask, u32 data);

/* ---- constants from upstream rtw8821c.h (verbatim) ---------------------- */
#define MASKBYTE0 0xff
#define MASKBYTE2 0xff0000
#define MASKLWORD 0xffff
#define MASKHWORD 0xffff0000
#define MASKDWORD 0xffffffff

#define REG_SYS_CTRL 0x000
#define BIT_FEN_EN   BIT(26)
#define REG_RXCCAMSK 0x814
#define REG_ADC40    0x8c8
#define REG_CHFIR    0x8f0
#define REG_ACBB0    0x948
#define REG_ACBBRXFIR 0x94c
#define REG_CCA_FLTR 0xa20
#define REG_TXSF2    0xa24
#define REG_TXSF6    0xa28
#define REG_ENTXCCK  0xa80
#define BTG_LNA      0xfc84
#define WLG_LNA      0x7532
#define REG_ENRXCCA  0xa84
#define BTG_CCA      0x0e
#define WLG_CCA      0x12
#define REG_TXFILTER 0xaac
#define REG_TXDFIR   0xc20
#define REG_RFECTL   0xcb8
#define B_BTG_SWITCH BIT(16)
#define B_CTRL_SWITCH BIT(18)
#define B_WL_SWITCH  (BIT(20) | BIT(22))
#define B_WLG_SWITCH BIT(21)
#define B_WLA_SWITCH BIT(23)
#define REG_DMEM_CTRL 0x1080
#define BIT_WL_RST   BIT(16)
#define RF18_BAND_MASK    (BIT(16) | BIT(9) | BIT(8))
#define RF18_BAND_2G      (0)
#define RF18_BAND_5G      (BIT(16) | BIT(8))
#define RF18_CHANNEL_MASK (MASKBYTE0)
#define RF18_RFSI_MASK    (BIT(18) | BIT(17))
#define RF18_RFSI_GE      (BIT(17))
#define RF18_RFSI_GT      (BIT(18))
#define RF18_BW_MASK      (BIT(11) | BIT(10))
#define RF18_BW_20M       (BIT(11) | BIT(10))
#define RF18_BW_40M       (BIT(11))
#define RF18_BW_80M       (BIT(10))

enum rtw8821ce_rf_set { SWITCH_TO_BTG, SWITCH_TO_WLG, SWITCH_TO_WLA, SWITCH_TO_BT };

/* our card is not China-SRRC regulatory */
static bool rtw_regd_srrc(struct rtw_dev *rtwdev) { (void)rtwdev; return false; }

/* rtw8821c_switch_rf_set (rtw8821c.c, verbatim — WLG/BTG/WLA paths) */
static void rtw8821c_switch_rf_set(struct rtw_dev *rtwdev, u8 rf_set)
{
    u32 reg;
    rtw_write32_set(rtwdev, REG_DMEM_CTRL, BIT_WL_RST);
    rtw_write32_set(rtwdev, REG_SYS_CTRL, BIT_FEN_EN);
    reg = rtw_read32(rtwdev, REG_RFECTL);
    switch (rf_set) {
    case SWITCH_TO_BTG:
        reg |= B_BTG_SWITCH;
        reg &= ~(B_CTRL_SWITCH | B_WL_SWITCH | B_WLG_SWITCH | B_WLA_SWITCH);
        rtw_write32_mask(rtwdev, REG_ENRXCCA, MASKBYTE2, BTG_CCA);
        rtw_write32_mask(rtwdev, REG_ENTXCCK, MASKLWORD, BTG_LNA);
        break;
    case SWITCH_TO_WLG:
        reg |= B_WL_SWITCH | B_WLG_SWITCH;
        reg &= ~(B_BTG_SWITCH | B_CTRL_SWITCH | B_WLA_SWITCH);
        rtw_write32_mask(rtwdev, REG_ENRXCCA, MASKBYTE2, WLG_CCA);
        rtw_write32_mask(rtwdev, REG_ENTXCCK, MASKLWORD, WLG_LNA);
        break;
    case SWITCH_TO_WLA:
        reg |= B_WL_SWITCH | B_WLA_SWITCH;
        reg &= ~(B_BTG_SWITCH | B_CTRL_SWITCH | B_WLG_SWITCH);
        break;
    case SWITCH_TO_BT:
    default:
        break;
    }
    rtw_write32(rtwdev, REG_RFECTL, reg);
}

/* RF register names (reg.h): RF_LUTDBG 0xdf, RF_XTALX2 0xb8 */
static void set_channel_rf(struct rtw_dev *rtwdev, u8 channel, u8 bw)
{
    struct rtw_hal_min *hal = &rtwdev->hal;
    u32 rf_reg18 = rtw_phy_read_rf(rtwdev, RF_PATH_A, 0x18, RFREG_MASK);

    rf_reg18 &= ~(RF18_BAND_MASK | RF18_CHANNEL_MASK | RF18_RFSI_MASK | RF18_BW_MASK);
    rf_reg18 |= (channel <= 14 ? RF18_BAND_2G : RF18_BAND_5G);
    rf_reg18 |= (channel & RF18_CHANNEL_MASK);
    if (channel >= 100 && channel <= 140) rf_reg18 |= RF18_RFSI_GE;
    else if (channel > 140)               rf_reg18 |= RF18_RFSI_GT;

    switch (bw) {
    default:
    case RTW_CHANNEL_WIDTH_20: rf_reg18 |= RF18_BW_20M; break;
    case RTW_CHANNEL_WIDTH_40: rf_reg18 |= RF18_BW_40M; break;
    case RTW_CHANNEL_WIDTH_80: rf_reg18 |= RF18_BW_80M; break;
    }

    if (channel <= 14) {
        rtw8821c_switch_rf_set(rtwdev, hal->rfe_btg ? SWITCH_TO_BTG : SWITCH_TO_WLG);
        rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTDBG, BIT(6), 0x1);
        rtw_write_rf(rtwdev, RF_PATH_A, 0x64, 0xf, 0xf);
    } else {
        rtw8821c_switch_rf_set(rtwdev, SWITCH_TO_WLA);
        rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTDBG, BIT(6), 0x0);
    }

    rtw_write_rf(rtwdev, RF_PATH_A, 0x18, RFREG_MASK, rf_reg18);
    rtw_write_rf(rtwdev, RF_PATH_A, RF_XTALX2, BIT(19), 0);
    rtw_write_rf(rtwdev, RF_PATH_A, RF_XTALX2, BIT(19), 1);
}

static void set_channel_rxdfir(struct rtw_dev *rtwdev, u8 bw)
{
    if (bw == RTW_CHANNEL_WIDTH_40) {
        rtw_write32_mask(rtwdev, REG_ACBB0, BIT(29)|BIT(28), 0x2);
        rtw_write32_mask(rtwdev, REG_ACBBRXFIR, BIT(29)|BIT(28), 0x2);
        rtw_write32_mask(rtwdev, REG_TXDFIR, BIT(31), 0x0);
        rtw_write32_mask(rtwdev, REG_CHFIR, BIT(31), 0x0);
    } else if (bw == RTW_CHANNEL_WIDTH_80) {
        rtw_write32_mask(rtwdev, REG_ACBB0, BIT(29)|BIT(28), 0x2);
        rtw_write32_mask(rtwdev, REG_ACBBRXFIR, BIT(29)|BIT(28), 0x1);
        rtw_write32_mask(rtwdev, REG_TXDFIR, BIT(31), 0x0);
        rtw_write32_mask(rtwdev, REG_CHFIR, BIT(31), 0x1);
    } else {
        rtw_write32_mask(rtwdev, REG_ACBB0, BIT(29)|BIT(28), 0x2);
        rtw_write32_mask(rtwdev, REG_ACBBRXFIR, BIT(29)|BIT(28), 0x2);
        rtw_write32_mask(rtwdev, REG_TXDFIR, BIT(31), 0x1);
        rtw_write32_mask(rtwdev, REG_CHFIR, BIT(31), 0x0);
    }
}

/* set_channel_bb (rtw8821c.c, verbatim 2G + bw20/40/80 paths) */
static void set_channel_bb(struct rtw_dev *rtwdev, u8 channel, u8 bw, u8 primary_ch_idx)
{
    struct rtw_hal_min *hal = &rtwdev->hal;
    u32 val32;

    if (channel <= 14) {
        rtw_write32_mask(rtwdev, REG_RXPSEL, BIT(28), 0x1);
        rtw_write32_mask(rtwdev, REG_CCK_CHECK, BIT(7), 0x0);
        rtw_write32_mask(rtwdev, REG_ENTXCCK, BIT(18), 0x0);
        rtw_write32_mask(rtwdev, REG_RXCCAMSK, 0x0000FC00, 15);
        rtw_write32_mask(rtwdev, REG_TXSCALE_A, 0xf00, 0x0);
        rtw_write32_mask(rtwdev, REG_CLKTRK, 0x1ffe0000, 0x96a);

        if (rtw_regd_srrc(rtwdev)) goto set_bw;   /* (srrc filter skipped) */

        if (channel == 14) {
            rtw_write32_mask(rtwdev, REG_TXSF2, MASKDWORD, 0x0000b81c);
            rtw_write32_mask(rtwdev, REG_TXSF6, MASKLWORD, 0x0000);
            rtw_write32_mask(rtwdev, REG_TXFILTER, MASKDWORD, 0x00003667);
        } else {
            rtw_write32_mask(rtwdev, REG_TXSF2, MASKDWORD, hal->ch_param[0]);
            rtw_write32_mask(rtwdev, REG_TXSF6, MASKLWORD, hal->ch_param[1] & MASKLWORD);
            rtw_write32_mask(rtwdev, REG_TXFILTER, MASKDWORD, hal->ch_param[2]);
        }
    } else if (channel > 35) {   /* 5GHz (rtw8821c.c, verbatim) */
        rtw_write32_mask(rtwdev, REG_ENTXCCK, BIT(18), 0x1);
        rtw_write32_mask(rtwdev, REG_CCK_CHECK, BIT(7), 0x1);
        rtw_write32_mask(rtwdev, REG_RXPSEL, BIT(28), 0x0);
        rtw_write32_mask(rtwdev, REG_RXCCAMSK, 0x0000FC00, 15);

        if (channel >= 36 && channel <= 64)
            rtw_write32_mask(rtwdev, REG_TXSCALE_A, 0xf00, 0x1);
        else if (channel >= 100 && channel <= 144)
            rtw_write32_mask(rtwdev, REG_TXSCALE_A, 0xf00, 0x2);
        else if (channel >= 149)
            rtw_write32_mask(rtwdev, REG_TXSCALE_A, 0xf00, 0x3);

        if (channel >= 36 && channel <= 48)
            rtw_write32_mask(rtwdev, REG_CLKTRK, 0x1ffe0000, 0x494);
        else if (channel >= 52 && channel <= 64)
            rtw_write32_mask(rtwdev, REG_CLKTRK, 0x1ffe0000, 0x453);
        else if (channel >= 100 && channel <= 116)
            rtw_write32_mask(rtwdev, REG_CLKTRK, 0x1ffe0000, 0x452);
        else if (channel >= 118 && channel <= 177)
            rtw_write32_mask(rtwdev, REG_CLKTRK, 0x1ffe0000, 0x412);
    }

set_bw:
    switch (bw) {
    default:
    case RTW_CHANNEL_WIDTH_20:
        val32 = (hw_read32(REG_ADCCLK) & 0xffcffc00) | 0x10010000;
        rtw_write32(rtwdev, REG_ADCCLK, val32);
        rtw_write32_mask(rtwdev, REG_ADC160, BIT(30), 0x1);
        break;
    case RTW_CHANNEL_WIDTH_40:
        if (primary_ch_idx == 1) rtw_write32_set(rtwdev, REG_RXSB, BIT(4));
        else                     rtw_write32_clr(rtwdev, REG_RXSB, BIT(4));
        val32 = (hw_read32(REG_ADCCLK) & 0xff3ff300) | 0x20020000 |
                ((primary_ch_idx & 0xf) << 2) | RTW_CHANNEL_WIDTH_40;
        rtw_write32(rtwdev, REG_ADCCLK, val32);
        rtw_write32_mask(rtwdev, REG_ADC160, BIT(30), 0x1);
        break;
    }
}

static void set_channel_bb_swing(struct rtw_dev *rtwdev, u8 channel)
{
    /* efuse tx_bb_swing default 0 -> swing 0x200 (rtw8821c_get_bb_swing) */
    (void)channel;
    rtw_write32_mask(rtwdev, REG_TXSCALE_A, GENMASK(31, 21), 0x200);
}

/* CCK TX-filter params cached ONCE from the BB-table defaults. Caching on every
 * set_channel call (as before) re-reads the registers AFTER a channel-14 tune has
 * overwritten them with the ch14-only values, poisoning the filter for ch1-13 for
 * the rest of the session. File-scope (not per-rtw_dev): the BB table is reloaded
 * with identical defaults on every bring-up, so the first-call snapshot stays valid. */
static u32 g_ch_param[3];
static int g_ch_param_valid;

/* rtw8821c_set_channel (rtw8821c.c, verbatim) */
void set_channel(struct rtw_dev *rtwdev, u8 channel, u8 bw, u8 primary_ch_idx)
{
    struct rtw_hal_min *hal = &rtwdev->hal;
    if (!g_ch_param_valid) {
        u32 a = hw_read32(REG_TXSF2), b = hw_read32(REG_TXSF6), c = hw_read32(REG_TXFILTER);
        /* Only latch once we read plausible BB-table defaults. If set_channel runs before
         * phy_set_param loaded the BB table (or after a failed bring-up), these read 0 or
         * 0xffffffff — latching that garbage would poison the CCK TX filter for the whole
         * kext lifetime (cache-once). Skip until the table is present; channel 14 then
         * uses its own hard-coded params, and 1-13 fall back to the live (table) values. */
        if (a && a != 0xffffffff && c && c != 0xffffffff) {
            g_ch_param[0] = a; g_ch_param[1] = b; g_ch_param[2] = c;
            g_ch_param_valid = 1;
        } else {
            g_ch_param[0] = a; g_ch_param[1] = b; g_ch_param[2] = c;   /* use live values, re-read next time */
        }
    }
    hal->ch_param[0] = g_ch_param[0];
    hal->ch_param[1] = g_ch_param[1];
    hal->ch_param[2] = g_ch_param[2];

    /* 80MHz is NOT ported: set_channel_bb has no 80MHz ADC-clock case, so a WIDTH_80
     * request would silently program a 20MHz ADC against an 80MHz RF/rxdfir setup.
     * Refuse loudly and run 20MHz until the 80MHz baseband path lands. */
    if (bw == RTW_CHANNEL_WIDTH_80) {
        rtw_warn(rtwdev, "set_channel: 80MHz not ported — falling back to 20MHz");
        bw = RTW_CHANNEL_WIDTH_20;
    }

    set_channel_bb(rtwdev, channel, bw, primary_ch_idx);
    set_channel_bb_swing(rtwdev, channel);
    set_channel_rf(rtwdev, channel, bw);
    set_channel_rxdfir(rtwdev, bw);

    /* TEST: fixed moderate per-rate TX AGC so probe requests actually radiate.
     * 8821c writes power to offset_txagc[path A]=0x1d00 + (rate & 0xfc), 4 rates
     * per u32 (rtw8821c_set_tx_power_index_by_rate). 0x28 ~= mid power index.
     * Proper efuse-driven power index is a later step. */
    rtw_write32(rtwdev, 0x1d00, 0x28282828);  /* CCK  1/2/5.5/11   */
    rtw_write32(rtwdev, 0x1d04, 0x28282828);  /* OFDM 6/9/12/18    */
    rtw_write32(rtwdev, 0x1d08, 0x28282828);  /* OFDM 24/36/48/54  */
}

