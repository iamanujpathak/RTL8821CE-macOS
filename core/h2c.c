/*
 * h2c.c — firmware H2C init handshake (general_info + phydm_info).
 *
 * After bring-up, upstream rtw_power_on sends these two H2C packets. phydm_info
 * in particular hands the firmware the RF/cut/antenna config, which starts the
 * firmware PHYDM that controls the T/R antenna switch + dynamic TX power. Our
 * theory: without it the transmit path stays gated (RX works, TX silent).
 *
 * H2C packet (32B, fw.h): word0 = category[6:0]|cmd_id[15:8]|sub_id[31:16],
 * word1 = total_len[15:0]|seq[31:16], word2 = payload. Sent via the H2C queue.
 */
#include <string.h>
#include "trx_regs.h"
#include "trx.h"

#define H2C_PKT_CATEGORY      0x01
#define H2C_PKT_CMD_ID        0xFF
#define H2C_PKT_GENERAL_INFO  0x0D
#define H2C_PKT_PHYDM_INFO    0x11
#define H2C_PKT_HDR_SIZE      8
#define FW_RF_1T1R            4

static void set_bits(u8 *pkt, int word, u32 val, u32 mask)
{
    u32 *w = (u32 *)pkt + word;
    int shift = __builtin_ctz(mask);
    *w = (*w & ~mask) | ((val << shift) & mask);
}

static void set_header(u8 *pkt, u8 sub_id)
{
    set_bits(pkt, 0, H2C_PKT_CATEGORY, GENMASK(6, 0));
    set_bits(pkt, 0, H2C_PKT_CMD_ID,   GENMASK(15, 8));
    set_bits(pkt, 0, sub_id,           GENMASK(31, 16));
}

void fw_handshake(struct rtw_dev *rtwdev)
{
    u8 pkt[32];

    /* general_info: FW TX boundary (rtw_fw_send_general_info) */
    memset(pkt, 0, sizeof(pkt));
    set_header(pkt, H2C_PKT_GENERAL_INFO);
    set_bits(pkt, 1, H2C_PKT_HDR_SIZE + 4, GENMASK(15, 0));      /* total_len */
    set_bits(pkt, 1, 0, GENMASK(31, 16));                        /* seq */
    u32 fw_tx_boundary = rtwdev->fifo.rsvd_fw_txbuf_addr - rtwdev->fifo.rsvd_boundary;
    set_bits(pkt, 2, fw_tx_boundary, GENMASK(23, 16));
    trx_tx_h2c(rtwdev, pkt);
    usleep_range(2000, 2000);

    /* phydm_info: rfe/rf-type/cut/antenna (rtw_fw_send_phydm_info) */
    memset(pkt, 0, sizeof(pkt));
    set_header(pkt, H2C_PKT_PHYDM_INFO);
    set_bits(pkt, 1, H2C_PKT_HDR_SIZE + 8, GENMASK(15, 0));
    set_bits(pkt, 1, 1, GENMASK(31, 16));                        /* seq */
    set_bits(pkt, 2, rtwdev->efuse.rfe_option, GENMASK(7, 0));   /* ref_type */
    set_bits(pkt, 2, FW_RF_1T1R, GENMASK(15, 8));                /* rf_type */
    set_bits(pkt, 2, rtwdev->hal.cut_version, GENMASK(23, 16));  /* cut_ver */
    set_bits(pkt, 2, 1, GENMASK(27, 24));                        /* rx ant (path A) */
    set_bits(pkt, 2, 1, GENMASK(31, 28));                        /* tx ant (path A) */
    trx_tx_h2c(rtwdev, pkt);
    usleep_range(2000, 2000);
}
