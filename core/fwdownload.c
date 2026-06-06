/*
 * fwdownload.c — RTL8821CE firmware download. DDMA core copied VERBATIM from
 * upstream rtw88 mac.c; the rsvd-page send leaf is routed to the bridge TRX ring
 * (trx_write_data_rsvd_page). The on-chip DDMA engine copies firmware from the
 * chip's TXBUF (filled via the BCN rsvd-page) into IMEM/DMEM, then we poll
 * MCUFW_CTRL for FW_READY.
 *
 * Verbatim (mac.c): wlan_cpu_enable, check_firmware_size, iddma_enable,
 * iddma_download_firmware, check_fw_checksum, download_firmware_to_mem,
 * start_download_firmware, download_firmware_reg_backup/reset_platform,
 * download_firmware_validate, download_firmware_end_flow.
 * Adapted for the harness: send_firmware_pkt + rtw_fw_write_data_rsvd_page (PCIe
 * path) call the bridge; the orchestrator skips ltecoex and uses trx_reset.
 */
#include <string.h>
#ifndef KERNEL
#include <stdlib.h>          /* malloc/free: kernel build gets them from rtw_shim.h */
#endif
#include "trx_regs.h"
#include "trx.h"

/* constants not in reg.h */
#define RTW_DMA_MAPPING_HIGH   3            /* main.h enum */
#ifndef ILLEGAL_KEY_GROUP
#define ILLEGAL_KEY_GROUP      0xFAAAAA00
#endif

/* check_hw_ready + rtw_read32_mask are shared (trx_regs.h) */

struct rtw_backup_info { u8 len; u32 reg; u32 val; };
static void rtw_restore_reg(struct rtw_dev *rtwdev, struct rtw_backup_info *bckp, u8 num)
{
    for (u8 i = 0; i < num; i++) {
        switch (bckp[i].len) {
        case 1: rtw_write8(rtwdev, bckp[i].reg, (u8)bckp[i].val);  break;
        case 2: rtw_write16(rtwdev, bckp[i].reg, (u16)bckp[i].val); break;
        case 4: rtw_write32(rtwdev, bckp[i].reg, bckp[i].val);     break;
        default: break;
        }
    }
}
#define WARN(cond, fmt, ...) do { if (cond) rtw_warn(rtwdev, fmt, ##__VA_ARGS__); } while (0)

/* =================== VERBATIM from upstream mac.c ======================== */

static void wlan_cpu_enable(struct rtw_dev *rtwdev, bool enable)
{
    if (enable) {
        rtw_write8_set(rtwdev, REG_RSV_CTRL + 1, BIT_WLMCU_IOIF);
        rtw_write8_set(rtwdev, REG_SYS_FUNC_EN + 1, BIT_FEN_CPUEN);
    } else {
        rtw_write8_clr(rtwdev, REG_SYS_FUNC_EN + 1, BIT_FEN_CPUEN);
        rtw_write8_clr(rtwdev, REG_RSV_CTRL + 1, BIT_WLMCU_IOIF);
    }
}

static bool check_firmware_size(const u8 *data, u32 size)
{
    const struct rtw_fw_hdr *fw_hdr = (const struct rtw_fw_hdr *)data;
    u32 dmem_size, imem_size, emem_size, real_size;

    dmem_size = le32_to_cpu(fw_hdr->dmem_size);
    imem_size = le32_to_cpu(fw_hdr->imem_size);
    emem_size = (fw_hdr->mem_usage & BIT(4)) ? le32_to_cpu(fw_hdr->emem_size) : 0;
    dmem_size += FW_HDR_CHKSUM_SIZE;
    imem_size += FW_HDR_CHKSUM_SIZE;
    emem_size += emem_size ? FW_HDR_CHKSUM_SIZE : 0;
    real_size = FW_HDR_SIZE + dmem_size + imem_size + emem_size;
    return real_size == size;
}

#define DLFW_RESTORE_REG_NUM 6

static void download_firmware_reg_backup(struct rtw_dev *rtwdev, struct rtw_backup_info *bckp)
{
    u8 tmp;
    u8 bckp_idx = 0;

    bckp[bckp_idx].len = 1;
    bckp[bckp_idx].reg = REG_TXDMA_PQ_MAP + 1;
    bckp[bckp_idx].val = rtw_read8(rtwdev, REG_TXDMA_PQ_MAP + 1);
    bckp_idx++;
    tmp = RTW_DMA_MAPPING_HIGH << 6;
    rtw_write8(rtwdev, REG_TXDMA_PQ_MAP + 1, tmp);

    bckp[bckp_idx].len = 1;
    bckp[bckp_idx].reg = REG_CR;
    bckp[bckp_idx].val = rtw_read8(rtwdev, REG_CR);
    bckp_idx++;
    bckp[bckp_idx].len = 4;
    bckp[bckp_idx].reg = REG_H2CQ_CSR;
    bckp[bckp_idx].val = BIT_H2CQ_FULL;
    bckp_idx++;
    tmp = BIT_HCI_TXDMA_EN | BIT_TXDMA_EN;
    rtw_write8(rtwdev, REG_CR, tmp);
    rtw_write32(rtwdev, REG_H2CQ_CSR, BIT_H2CQ_FULL);

    bckp[bckp_idx].len = 2;
    bckp[bckp_idx].reg = REG_FIFOPAGE_INFO_1;
    bckp[bckp_idx].val = rtw_read16(rtwdev, REG_FIFOPAGE_INFO_1);
    bckp_idx++;
    bckp[bckp_idx].len = 4;
    bckp[bckp_idx].reg = REG_RQPN_CTRL_2;
    bckp[bckp_idx].val = rtw_read32(rtwdev, REG_RQPN_CTRL_2) | BIT_LD_RQPN;
    bckp_idx++;
    rtw_write16(rtwdev, REG_FIFOPAGE_INFO_1, 0x200);
    rtw_write32(rtwdev, REG_RQPN_CTRL_2, bckp[bckp_idx - 1].val);

    tmp = rtw_read8(rtwdev, REG_BCN_CTRL);
    bckp[bckp_idx].len = 1;
    bckp[bckp_idx].reg = REG_BCN_CTRL;
    bckp[bckp_idx].val = tmp;
    bckp_idx++;
    tmp = (u8)((tmp & (~BIT_EN_BCN_FUNCTION)) | BIT_DIS_TSF_UDT);
    rtw_write8(rtwdev, REG_BCN_CTRL, tmp);

    WARN(bckp_idx != DLFW_RESTORE_REG_NUM, "wrong backup number\n");
}

static void download_firmware_reset_platform(struct rtw_dev *rtwdev)
{
    rtw_write8_clr(rtwdev, REG_CPU_DMEM_CON + 2, BIT_WL_PLATFORM_RST >> 16);
    rtw_write8_clr(rtwdev, REG_SYS_CLK_CTRL + 1, BIT_CPU_CLK_EN >> 8);
    rtw_write8_set(rtwdev, REG_CPU_DMEM_CON + 2, BIT_WL_PLATFORM_RST >> 16);
    rtw_write8_set(rtwdev, REG_SYS_CLK_CTRL + 1, BIT_CPU_CLK_EN >> 8);
}

static int iddma_enable(struct rtw_dev *rtwdev, u32 src, u32 dst, u32 ctrl)
{
    rtw_write32(rtwdev, REG_DDMA_CH0SA, src);
    rtw_write32(rtwdev, REG_DDMA_CH0DA, dst);
    rtw_write32(rtwdev, REG_DDMA_CH0CTRL, ctrl);
    if (!check_hw_ready(rtwdev, REG_DDMA_CH0CTRL, BIT_DDMACH0_OWN, 0))
        return -EBUSY;
    return 0;
}

static int iddma_download_firmware(struct rtw_dev *rtwdev, u32 src, u32 dst, u32 len, u8 first)
{
    u32 ch0_ctrl = BIT_DDMACH0_CHKSUM_EN | BIT_DDMACH0_OWN;

    if (!check_hw_ready(rtwdev, REG_DDMA_CH0CTRL, BIT_DDMACH0_OWN, 0))
        return -EBUSY;
    ch0_ctrl |= len & BIT_MASK_DDMACH0_DLEN;
    if (!first)
        ch0_ctrl |= BIT_DDMACH0_CHKSUM_CONT;
    if (iddma_enable(rtwdev, src, dst, ch0_ctrl))
        return -EBUSY;
    return 0;
}

static bool check_fw_checksum(struct rtw_dev *rtwdev, u32 addr)
{
    u8 fw_ctrl = rtw_read8(rtwdev, REG_MCUFW_CTRL);

    if (rtw_read32(rtwdev, REG_DDMA_CH0CTRL) & BIT_DDMACH0_CHKSUM_STS) {
        if (addr < OCPBASE_DMEM_88XX) {
            fw_ctrl |= BIT_IMEM_DW_OK;
            fw_ctrl &= ~BIT_IMEM_CHKSUM_OK;
            rtw_write8(rtwdev, REG_MCUFW_CTRL, fw_ctrl);
        } else {
            fw_ctrl |= BIT_DMEM_DW_OK;
            fw_ctrl &= ~BIT_DMEM_CHKSUM_OK;
            rtw_write8(rtwdev, REG_MCUFW_CTRL, fw_ctrl);
        }
        rtw_err(rtwdev, "invalid fw checksum\n");
        return false;
    }
    if (addr < OCPBASE_DMEM_88XX) {
        fw_ctrl |= (BIT_IMEM_DW_OK | BIT_IMEM_CHKSUM_OK);
        rtw_write8(rtwdev, REG_MCUFW_CTRL, fw_ctrl);
    } else {
        fw_ctrl |= (BIT_DMEM_DW_OK | BIT_DMEM_CHKSUM_OK);
        rtw_write8(rtwdev, REG_MCUFW_CTRL, fw_ctrl);
    }
    return true;
}

/* the send leaf — routed to the bridge TRX BCN ring (see below) */
static int send_firmware_pkt(struct rtw_dev *rtwdev, u16 pg_addr, const u8 *data, u32 size);

static int download_firmware_to_mem(struct rtw_dev *rtwdev, const u8 *data,
                                    u32 src, u32 dst, u32 size)
{
    const struct rtw_chip_info *chip = rtwdev->chip;
    u32 desc_size = chip->tx_pkt_desc_sz;
    u8 first_part;
    u32 mem_offset, residue_size, pkt_size;
    u32 max_size = 0x1000;
    u32 val;
    int ret;

    mem_offset = 0;
    first_part = 1;
    residue_size = size;

    val = rtw_read32(rtwdev, REG_DDMA_CH0CTRL);
    val |= BIT_DDMACH0_RESET_CHKSUM_STS;
    rtw_write32(rtwdev, REG_DDMA_CH0CTRL, val);

    while (residue_size) {
        pkt_size = (residue_size >= max_size) ? max_size : residue_size;

        ret = send_firmware_pkt(rtwdev, (u16)(src >> 7), data + mem_offset, pkt_size);
        if (ret)
            return ret;

        ret = iddma_download_firmware(rtwdev, OCPBASE_TXBUF_88XX + src + desc_size,
                                      dst + mem_offset, pkt_size, first_part);
        if (ret)
            return ret;

        first_part = 0;
        mem_offset += pkt_size;
        residue_size -= pkt_size;
    }

    if (!check_fw_checksum(rtwdev, dst))
        return -EINVAL;
    return 0;
}

static int start_download_firmware(struct rtw_dev *rtwdev, const u8 *data, u32 size)
{
    const struct rtw_fw_hdr *fw_hdr = (const struct rtw_fw_hdr *)data;
    const u8 *cur_fw;
    u16 val;
    u32 imem_size, dmem_size, emem_size, addr;
    int ret;

    dmem_size = le32_to_cpu(fw_hdr->dmem_size);
    imem_size = le32_to_cpu(fw_hdr->imem_size);
    emem_size = (fw_hdr->mem_usage & BIT(4)) ? le32_to_cpu(fw_hdr->emem_size) : 0;
    dmem_size += FW_HDR_CHKSUM_SIZE;
    imem_size += FW_HDR_CHKSUM_SIZE;
    emem_size += emem_size ? FW_HDR_CHKSUM_SIZE : 0;

    val = (u16)(rtw_read16(rtwdev, REG_MCUFW_CTRL) & 0x3800);
    val |= BIT_MCUFWDL_EN;
    rtw_write16(rtwdev, REG_MCUFW_CTRL, val);

    cur_fw = data + FW_HDR_SIZE;
    addr = le32_to_cpu(fw_hdr->dmem_addr);
    addr &= ~BIT(31);
    ret = download_firmware_to_mem(rtwdev, cur_fw, 0, addr, dmem_size);
    if (ret)
        return ret;

    cur_fw = data + FW_HDR_SIZE + dmem_size;
    addr = le32_to_cpu(fw_hdr->imem_addr);
    addr &= ~BIT(31);
    ret = download_firmware_to_mem(rtwdev, cur_fw, 0, addr, imem_size);
    if (ret)
        return ret;

    if (emem_size) {
        cur_fw = data + FW_HDR_SIZE + dmem_size + imem_size;
        addr = le32_to_cpu(fw_hdr->emem_addr);
        addr &= ~BIT(31);
        ret = download_firmware_to_mem(rtwdev, cur_fw, 0, addr, emem_size);
        if (ret)
            return ret;
    }
    return 0;
}

static int download_firmware_validate(struct rtw_dev *rtwdev)
{
    u32 fw_key;

    if (!check_hw_ready(rtwdev, REG_MCUFW_CTRL, FW_READY_MASK, FW_READY)) {
        fw_key = rtw_read32(rtwdev, REG_FW_DBG7) & FW_KEY_MASK;
        if (fw_key == ILLEGAL_KEY_GROUP)
            rtw_err(rtwdev, "invalid fw key\n");
        return -EINVAL;
    }
    return 0;
}

static void download_firmware_end_flow(struct rtw_dev *rtwdev)
{
    u16 fw_ctrl;

    rtw_write32(rtwdev, REG_TXDMA_STATUS, BTI_PAGE_OVF);
    fw_ctrl = rtw_read16(rtwdev, REG_MCUFW_CTRL);
    if ((fw_ctrl & BIT_CHECK_SUM_OK) != BIT_CHECK_SUM_OK)
        return;
    fw_ctrl = (fw_ctrl | BIT_FW_DW_RDY) & ~BIT_MCUFWDL_EN;
    rtw_write16(rtwdev, REG_MCUFW_CTRL, fw_ctrl);
}

/* ============ adapted leaf: rsvd-page write over the bridge =============== */

/* PCIe path of upstream rtw_fw_write_data_rsvd_page (BCN-ctrl dance + bcn_valid
 * poll). rsvd_boundary is 0 in our harness (set in a later stage). */
static int rtw_fw_write_data_rsvd_page(struct rtw_dev *rtwdev, u16 pg_addr, u8 *buf, u32 size)
{
    u8 bckp[3];
    u8 val;
    int ret;

    if (!size)
        return -EINVAL;

    bckp[2] = rtw_read8(rtwdev, REG_BCN_CTRL);

    pg_addr &= BIT_MASK_BCN_HEAD_1_V1;
    pg_addr |= BIT_BCN_VALID_V1;
    rtw_write16(rtwdev, REG_FIFOPAGE_CTRL_2, pg_addr);

    val = rtw_read8(rtwdev, REG_CR + 1);
    bckp[0] = val;
    val |= BIT_ENSWBCN >> 8;
    rtw_write8(rtwdev, REG_CR + 1, val);

    rtw_write8(rtwdev, REG_BCN_CTRL, (bckp[2] & ~BIT_EN_BCN_FUNCTION) | BIT_DIS_TSF_UDT);

    val = rtw_read8(rtwdev, REG_FWHW_TXQ_CTRL + 2);
    bckp[1] = val;
    val &= ~(BIT_EN_BCNQ_DL >> 16);
    rtw_write8(rtwdev, REG_FWHW_TXQ_CTRL + 2, val);

    ret = trx_write_data_rsvd_page(rtwdev, buf, size);
    if (ret) {
        rtw_err(rtwdev, "failed to write data to rsvd page\n");
        goto restore;
    }

    if (!check_hw_ready(rtwdev, REG_FIFOPAGE_CTRL_2, BIT_BCN_VALID_V1, 1)) {
        rtw_err(rtwdev, "error beacon valid\n");
        ret = -EBUSY;
    }

restore:
    rtw_write16(rtwdev, REG_FIFOPAGE_CTRL_2, 0 | BIT_BCN_VALID_V1);
    rtw_write8(rtwdev, REG_BCN_CTRL, bckp[2]);
    rtw_write8(rtwdev, REG_FWHW_TXQ_CTRL + 2, bckp[1]);
    rtw_write8(rtwdev, REG_CR + 1, bckp[0]);
    return ret;
}

static int send_firmware_pkt(struct rtw_dev *rtwdev, u16 pg_addr, const u8 *data, u32 size)
{
    u8 *buf = (u8 *)malloc(size);
    int ret;
    if (!buf)
        return -ENOMEM;
    memcpy(buf, data, size);
    ret = rtw_fw_write_data_rsvd_page(rtwdev, pg_addr, buf, size);
    free(buf);
    if (ret)
        rtw_err(rtwdev, "failed to download rsvd page\n");
    return ret;
}

/* ============ orchestrator (mirrors __rtw_download_firmware) ============== */

int download_firmware(struct rtw_dev *rtwdev, const u8 *data, u32 size)
{
    struct rtw_backup_info bckp[DLFW_RESTORE_REG_NUM];
    int ret;

    if (!check_firmware_size(data, size)) {
        rtw_err(rtwdev, "firmware size check failed");
        return -EINVAL;
    }

    wlan_cpu_enable(rtwdev, false);

    download_firmware_reg_backup(rtwdev, bckp);
    download_firmware_reset_platform(rtwdev);

    ret = start_download_firmware(rtwdev, data, size);
    if (ret)
        goto dlfw_fail;

    rtw_restore_reg(rtwdev, bckp, DLFW_RESTORE_REG_NUM);
    download_firmware_end_flow(rtwdev);

    wlan_cpu_enable(rtwdev, true);

    ret = download_firmware_validate(rtwdev);
    if (ret)
        goto dlfw_fail;

    /* reset desc and index (upstream calls rtw_hci_setup) */
    trx_reset(rtwdev);

    set_bit(RTW_FLAG_FW_RUNNING, rtwdev->flags);
    return 0;

dlfw_fail:
    rtw_write8_clr(rtwdev, REG_MCUFW_CTRL, BIT_MCUFWDL_EN);
    rtw_write8_set(rtwdev, REG_SYS_FUNC_EN + 1, BIT_FEN_CPUEN);
    return ret;
}
