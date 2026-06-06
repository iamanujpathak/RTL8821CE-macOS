/*
 * efuse.c — read the chip EFUSE (over the bridge).
 *
 * VERBATIM from upstream rtw88 efuse.c (rtw_dump_physical_efuse_map RLE-source
 * read + rtw_dump_logical_efuse_map RLE decode) + rtw8821c.c (cfg_ldo25). The
 * grant-on/off is the REG_EFUSE_ACCESS=0x69 write the probe already proved.
 *
 * Upstream reads EFUSE in its own brief power-on/off at probe time; our MAC is
 * already powered, so we read directly (no power toggle). Fields
 * are extracted at their struct rtw8821c_efuse offsets (rtw8821c.h):
 *   crystal_cap (xtal_k) @ 0xB9, rf_board_option @ 0xC1, rfe_option @ 0xCA,
 *   channel_plan @ 0xB8, and the PCIe MAC (map->e.mac_addr) @ 0xD0.
 */
#ifndef KERNEL
#include <stdlib.h>          /* malloc/free: kernel build gets them from rtw_shim.h */
#endif
#include <string.h>
#include "trx_regs.h"

/* EFUSE logical-map field offsets (struct rtw8821c_efuse, rtw8821c.h) */
#define EF_OFF_CHANNEL_PLAN 0xB8
#define EF_OFF_XTAL_K       0xB9
#define EF_OFF_RF_BOARD_OPT 0xC1
#define EF_OFF_RFE_OPTION   0xCA
#define EF_OFF_MAC_PCIE     0xD0
#define EFUSE_READ_FAIL     0xff

/* rtw8821c_cfg_ldo25 (rtw8821c.c, verbatim) */
static void cfg_ldo25(struct rtw_dev *rtwdev, bool enable)
{
    u8 ldo_pwr = rtw_read8(rtwdev, REG_LDO_EFUSE_CTRL + 3);
    ldo_pwr = enable ? ldo_pwr | BIT(7) : ldo_pwr & ~BIT(7);
    rtw_write8(rtwdev, REG_LDO_EFUSE_CTRL + 3, ldo_pwr);
}

static void grant_on(struct rtw_dev *rtwdev)  { rtw_write8(rtwdev, REG_EFUSE_ACCESS, EFUSE_ACCESS_ON); }
static void grant_off(struct rtw_dev *rtwdev) { rtw_write8(rtwdev, REG_EFUSE_ACCESS, 0x00); }

static void switch_efuse_bank(struct rtw_dev *rtwdev)
{
    rtw_write32_mask(rtwdev, REG_LDO_EFUSE_CTRL, BIT_MASK_EFUSE_BANK_SEL, 0 /*WIFI*/);
}

/* rtw_dump_physical_efuse_map (efuse.c, verbatim) */
static int dump_physical_efuse_map(struct rtw_dev *rtwdev, u8 *map)
{
    u32 size = rtwdev->chip->phy_efuse_size;
    u32 efuse_ctl, addr, cnt;

    grant_on(rtwdev);
    switch_efuse_bank(rtwdev);
    cfg_ldo25(rtwdev, false);                 /* disable 2.5V LDO */

    efuse_ctl = rtw_read32(rtwdev, REG_EFUSE_CTRL);
    for (addr = 0; addr < size; addr++) {
        efuse_ctl &= ~(BIT_MASK_EF_DATA | BITS_EF_ADDR);
        efuse_ctl |= (addr & BIT_MASK_EF_ADDR) << BIT_SHIFT_EF_ADDR;
        rtw_write32(rtwdev, REG_EFUSE_CTRL, efuse_ctl & (~BIT_EF_FLAG));

        cnt = 1000000;
        do {
            udelay(1);
            efuse_ctl = rtw_read32(rtwdev, REG_EFUSE_CTRL);
            if (--cnt == 0) { grant_off(rtwdev); return -EBUSY; }
        } while (!(efuse_ctl & BIT_EF_FLAG));

        *(map + addr) = (u8)(efuse_ctl & BIT_MASK_EF_DATA);
    }
    grant_off(rtwdev);
    return 0;
}

/* helpers + rtw_dump_logical_efuse_map (efuse.c, verbatim) */
#define invalid_efuse_header(hdr1, hdr2) \
    ((hdr1) == 0xff || (((hdr1) & 0x1f) == 0xf && (hdr2) == 0xff))
#define invalid_efuse_content(word_en, i) (((word_en) & BIT(i)) != 0x0)
#define get_efuse_blk_idx_2_byte(hdr1, hdr2) \
    ((((hdr2) & 0xf0) >> 1) | (((hdr1) >> 5) & 0x07))
#define get_efuse_blk_idx_1_byte(hdr1) (((hdr1) & 0xf0) >> 4)
#define block_idx_to_logical_idx(blk_idx, i) (((blk_idx) << 3) + ((i) << 1))

static int dump_logical_efuse_map(struct rtw_dev *rtwdev, u8 *phy_map, u8 *log_map)
{
    u32 physical_size = rtwdev->chip->phy_efuse_size;
    u32 protect_size = rtwdev->chip->ptct_efuse_size;
    u32 logical_size = rtwdev->chip->log_efuse_size;
    u32 phy_idx, log_idx;
    u8 hdr1, hdr2, blk_idx, word_en;
    int i;

    for (phy_idx = 0; phy_idx < physical_size - protect_size;) {
        hdr1 = phy_map[phy_idx];
        hdr2 = phy_map[phy_idx + 1];
        if (invalid_efuse_header(hdr1, hdr2))
            break;

        if ((hdr1 & 0x1f) == 0xf) {
            blk_idx = get_efuse_blk_idx_2_byte(hdr1, hdr2);
            word_en = hdr2 & 0xf;
            phy_idx += 2;
        } else {
            blk_idx = get_efuse_blk_idx_1_byte(hdr1);
            word_en = hdr1 & 0xf;
            phy_idx += 1;
        }

        for (i = 0; i < 4; i++) {
            if (invalid_efuse_content(word_en, i))
                continue;
            log_idx = block_idx_to_logical_idx(blk_idx, i);
            if (phy_idx + 1 > physical_size - protect_size || log_idx + 1 > logical_size)
                return -EINVAL;
            log_map[log_idx] = phy_map[phy_idx];
            log_map[log_idx + 1] = phy_map[phy_idx + 1];
            phy_idx += 2;
        }
    }
    return 0;
}

/* orchestrator: dump -> decode -> extract fields into rtwdev->efuse */
int efuse_read(struct rtw_dev *rtwdev)
{
    u32 phy_size = rtwdev->chip->phy_efuse_size;
    u32 log_size = rtwdev->chip->log_efuse_size;
    u8 *phy_map = (u8 *)malloc(phy_size);
    u8 *log_map = (u8 *)malloc(log_size);
    int ret = -ENOMEM;

    if (!phy_map || !log_map)
        goto out;

    ret = dump_physical_efuse_map(rtwdev, phy_map);
    if (ret) { rtw_err(rtwdev, "dump physical efuse failed"); goto out; }

    memset(log_map, 0xff, log_size);
    ret = dump_logical_efuse_map(rtwdev, phy_map, log_map);
    if (ret) { rtw_err(rtwdev, "dump logical efuse failed"); goto out; }

    /* extract (rtw8821c_read_efuse + rtw8821ce_efuse_parsing) */
    rtwdev->efuse.rtl_id = (u16)(log_map[0] | (log_map[1] << 8));  /* offset 0x00 */
    memcpy(rtwdev->efuse.addr, &log_map[EF_OFF_MAC_PCIE], 6);
    rtwdev->efuse.rfe_option      = log_map[EF_OFF_RFE_OPTION] & 0x1f;
    rtwdev->efuse.crystal_cap     = log_map[EF_OFF_XTAL_K];
    rtwdev->efuse.rf_board_option = log_map[EF_OFF_RF_BOARD_OPT];
    rtwdev->efuse.channel_plan    = log_map[EF_OFF_CHANNEL_PLAN];
    if (rtwdev->efuse.crystal_cap == EFUSE_READ_FAIL)
        rtwdev->efuse.crystal_cap = 0;
    if (rtwdev->efuse.channel_plan == EFUSE_READ_FAIL)
        rtwdev->efuse.channel_plan = 0x7f;
    if (rtwdev->efuse.rf_board_option == EFUSE_READ_FAIL)
        rtwdev->efuse.rf_board_option = 0;
    rtwdev->efuse.btcoex = (rtwdev->efuse.rf_board_option & 0xe0) == 0x20;

    /* transparency: dump the parsed field region so decode alignment is visible.
     * 0xB8 chan_plan, 0xB9 xtal, 0xC1 board, 0xCA rfe, 0xD0..D5 MAC. Structured
     * (non-0xff) data here = the physical read + RLE decode landed correctly. */
    printf("  efuse[0xB8..0xD5]:");
    for (u32 o = 0xB8; o <= 0xD5; o++) printf(" %02x", log_map[o]);
    printf("\n");
    ret = 0;

out:
    free(log_map);
    free(phy_map);
    return ret;
}
