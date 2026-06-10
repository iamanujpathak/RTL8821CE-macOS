/*
 * macinit.c — rtw_mac_init (post-firmware MAC config).
 * VERBATIM from upstream rtw88 mac.c (txdma_queue_mapping, rtw_set_trx_fifo_info,
 * __priority_queue_cfg, priority_queue_cfg, init_h2c, rtw_init_trx_cfg,
 * rtw_drv_info_cfg) + rtw8821c.c (rtw8821c_mac_init) + the page/rqpn tables and
 * WLAN_* constants (rtw8821c.h). Register access routes to the bridge via the
 * shim. This computes the real rsvd_boundary.
 */
#include "trx_regs.h"

/* mac.h rsvd-page page counts */
#define RSVD_PG_H2C_EXTRAINFO_NUM   24
#define RSVD_PG_H2C_STATICINFO_NUM  8
#define RSVD_PG_H2CQ_NUM            8
#define RSVD_PG_CPU_INSTRUCTION_NUM 0
#define RSVD_PG_FW_TXBUF_NUM        4
#define C2H_PKT_BUF                 256
#define PHY_STATUS_SIZE             4
#define TX_PAGE_SIZE_SHIFT          7

/* ---- WLAN_* constants (rtw8821c.h, verbatim) ---------------------------- */
#define WLAN_SLOT_TIME		0x09
#define WLAN_PIFS_TIME		0x19
#define WLAN_SIFS_CCK_CONT_TX	0xA
#define WLAN_SIFS_OFDM_CONT_TX	0xE
#define WLAN_SIFS_CCK_TRX	0x10
#define WLAN_SIFS_OFDM_TRX	0x10
#define WLAN_VO_TXOP_LIMIT	0x186
#define WLAN_VI_TXOP_LIMIT	0x3BC
#define WLAN_RDG_NAV		0x05
#define WLAN_TXOP_NAV		0x1B
#define WLAN_CCK_RX_TSF		0x30
#define WLAN_OFDM_RX_TSF	0x30
#define WLAN_TBTT_PROHIBIT	0x04
#define WLAN_TBTT_HOLD_TIME	0x064
#define WLAN_DRV_EARLY_INT	0x04
#define WLAN_BCN_DMA_TIME	0x02
#define WLAN_RX_FILTER0		0x0FFFFFFF
#define WLAN_RX_FILTER2		0xFFFF
#define WLAN_RCR_CFG		0xE400220E
#define WLAN_RXPKT_MAX_SZ	12288
#define WLAN_RXPKT_MAX_SZ_512	(WLAN_RXPKT_MAX_SZ >> 9)
#define WLAN_AMPDU_MAX_TIME		0x70
#define WLAN_RTS_LEN_TH			0xFF
#define WLAN_RTS_TX_TIME_TH		0x08
#define WLAN_MAX_AGG_PKT_LIMIT		0x20
#define WLAN_RTS_MAX_AGG_PKT_LIMIT	0x20
#define FAST_EDCA_VO_TH		0x06
#define FAST_EDCA_VI_TH		0x06
#define FAST_EDCA_BE_TH		0x06
#define FAST_EDCA_BK_TH		0x06
#define WLAN_BAR_RETRY_LIMIT		0x01
#define WLAN_RA_TRY_RATE_AGG_LIMIT	0x08
#define WLAN_TX_FUNC_CFG1		0x30
#define WLAN_TX_FUNC_CFG2		0x30
#define WLAN_MAC_OPT_NORM_FUNC1		0x98
#define WLAN_MAC_OPT_LB_FUNC1		0x80
#define WLAN_MAC_OPT_FUNC2		0xb0810041
#define WLAN_SIFS_CFG	(WLAN_SIFS_CCK_CONT_TX | \
			(WLAN_SIFS_OFDM_CONT_TX << BIT_SHIFT_SIFS_OFDM_CTX) | \
			(WLAN_SIFS_CCK_TRX << BIT_SHIFT_SIFS_CCK_TRX) | \
			(WLAN_SIFS_OFDM_TRX << BIT_SHIFT_SIFS_OFDM_TRX))
#define WLAN_TBTT_TIME	(WLAN_TBTT_PROHIBIT |\
			(WLAN_TBTT_HOLD_TIME << BIT_SHIFT_TBTT_HOLD_TIME_AP))
#define WLAN_NAV_CFG		(WLAN_RDG_NAV | (WLAN_TXOP_NAV << 16))
#define WLAN_RX_TSF_CFG		(WLAN_CCK_RX_TSF | (WLAN_OFDM_RX_TSF) << 8)
#define WLAN_PRE_TXCNT_TIME_TH		0x1E4
#define REG_INIRTS_RATE_SEL		0x0480   /* rtw8821c.h */

/* ---- 8821c page/rqpn tables (rtw8821c.c, verbatim) ---------------------- */
const struct rtw_page_table page_table_8821c[] = {
	{16, 16, 16, 14, 1},
	{16, 16, 16, 14, 1},
	{16, 16, 0, 0, 1},
	{16, 16, 16, 0, 1},
	{16, 16, 16, 14, 1},
};
const struct rtw_rqpn rqpn_table_8821c[] = {
	{RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_LOW, RTW_DMA_MAPPING_LOW,
	 RTW_DMA_MAPPING_EXTRA, RTW_DMA_MAPPING_HIGH},
	{RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_LOW, RTW_DMA_MAPPING_LOW,
	 RTW_DMA_MAPPING_EXTRA, RTW_DMA_MAPPING_HIGH},
	{RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_HIGH,
	 RTW_DMA_MAPPING_HIGH, RTW_DMA_MAPPING_HIGH},
	{RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_LOW, RTW_DMA_MAPPING_LOW,
	 RTW_DMA_MAPPING_HIGH, RTW_DMA_MAPPING_HIGH},
	{RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_LOW, RTW_DMA_MAPPING_LOW,
	 RTW_DMA_MAPPING_EXTRA, RTW_DMA_MAPPING_HIGH},
};

/* =================== VERBATIM from upstream mac.c ======================== */

static int txdma_queue_mapping(struct rtw_dev *rtwdev)
{
	const struct rtw_chip_info *chip = rtwdev->chip;
	const struct rtw_rqpn *rqpn = NULL;
	u16 txdma_pq_map = 0;

	switch (rtw_hci_type(rtwdev)) {
	case RTW_HCI_TYPE_PCIE:
		rqpn = &chip->rqpn_table[1];
		break;
	default:
		return -EINVAL;
	}

	rtwdev->fifo.rqpn = rqpn;
	txdma_pq_map |= BIT_TXDMA_HIQ_MAP(rqpn->dma_map_hi);
	txdma_pq_map |= BIT_TXDMA_MGQ_MAP(rqpn->dma_map_mg);
	txdma_pq_map |= BIT_TXDMA_BKQ_MAP(rqpn->dma_map_bk);
	txdma_pq_map |= BIT_TXDMA_BEQ_MAP(rqpn->dma_map_be);
	txdma_pq_map |= BIT_TXDMA_VIQ_MAP(rqpn->dma_map_vi);
	txdma_pq_map |= BIT_TXDMA_VOQ_MAP(rqpn->dma_map_vo);
	rtw_write16(rtwdev, REG_TXDMA_PQ_MAP, txdma_pq_map);

	rtw_write8(rtwdev, REG_CR, 0);
	rtw_write8(rtwdev, REG_CR, MAC_TRX_ENABLE);
	if (rtw_chip_wcpu_3081(rtwdev))
		rtw_write32(rtwdev, REG_H2CQ_CSR, BIT_H2CQ_FULL);

	return 0;
}

int rtw_set_trx_fifo_info(struct rtw_dev *rtwdev)
{
	const struct rtw_chip_info *chip = rtwdev->chip;
	struct rtw_fifo_conf *fifo = &rtwdev->fifo;
	u16 cur_pg_addr;
	u8 csi_buf_pg_num = chip->csi_buf_pg_num;

	fifo->rsvd_drv_pg_num = chip->rsvd_drv_pg_num;
	fifo->txff_pg_num = chip->txff_size / chip->page_size;
	fifo->rsvd_pg_num = fifo->rsvd_drv_pg_num +
			   RSVD_PG_H2C_EXTRAINFO_NUM +
			   RSVD_PG_H2C_STATICINFO_NUM +
			   RSVD_PG_H2CQ_NUM +
			   RSVD_PG_CPU_INSTRUCTION_NUM +
			   RSVD_PG_FW_TXBUF_NUM +
			   csi_buf_pg_num;

	if (fifo->rsvd_pg_num > fifo->txff_pg_num)
		return -ENOMEM;

	fifo->acq_pg_num = fifo->txff_pg_num - fifo->rsvd_pg_num;
	fifo->rsvd_boundary = fifo->txff_pg_num - fifo->rsvd_pg_num;

	cur_pg_addr = fifo->txff_pg_num;
	cur_pg_addr -= csi_buf_pg_num;
	fifo->rsvd_csibuf_addr = cur_pg_addr;
	cur_pg_addr -= RSVD_PG_FW_TXBUF_NUM;
	fifo->rsvd_fw_txbuf_addr = cur_pg_addr;
	cur_pg_addr -= RSVD_PG_CPU_INSTRUCTION_NUM;
	fifo->rsvd_cpu_instr_addr = cur_pg_addr;
	cur_pg_addr -= RSVD_PG_H2CQ_NUM;
	fifo->rsvd_h2cq_addr = cur_pg_addr;
	cur_pg_addr -= RSVD_PG_H2C_STATICINFO_NUM;
	fifo->rsvd_h2c_sta_info_addr = cur_pg_addr;
	cur_pg_addr -= RSVD_PG_H2C_EXTRAINFO_NUM;
	fifo->rsvd_h2c_info_addr = cur_pg_addr;
	cur_pg_addr -= fifo->rsvd_drv_pg_num;
	fifo->rsvd_drv_addr = cur_pg_addr;

	if (fifo->rsvd_boundary != fifo->rsvd_drv_addr) {
		rtw_err(rtwdev, "wrong rsvd driver address\n");
		return -EINVAL;
	}
	return 0;
}

static int __priority_queue_cfg(struct rtw_dev *rtwdev,
				const struct rtw_page_table *pg_tbl, u16 pubq_num)
{
	const struct rtw_chip_info *chip = rtwdev->chip;
	struct rtw_fifo_conf *fifo = &rtwdev->fifo;

	rtw_write16(rtwdev, REG_FIFOPAGE_INFO_1, pg_tbl->hq_num);
	rtw_write16(rtwdev, REG_FIFOPAGE_INFO_2, pg_tbl->lq_num);
	rtw_write16(rtwdev, REG_FIFOPAGE_INFO_3, pg_tbl->nq_num);
	rtw_write16(rtwdev, REG_FIFOPAGE_INFO_4, pg_tbl->exq_num);
	rtw_write16(rtwdev, REG_FIFOPAGE_INFO_5, pubq_num);
	rtw_write32_set(rtwdev, REG_RQPN_CTRL_2, BIT_LD_RQPN);

	rtw_write16(rtwdev, REG_FIFOPAGE_CTRL_2, fifo->rsvd_boundary);
	rtw_write8_set(rtwdev, REG_FWHW_TXQ_CTRL + 2, BIT_EN_WR_FREE_TAIL >> 16);

	rtw_write16(rtwdev, REG_BCNQ_BDNY_V1, fifo->rsvd_boundary);
	rtw_write16(rtwdev, REG_FIFOPAGE_CTRL_2 + 2, fifo->rsvd_boundary);
	rtw_write16(rtwdev, REG_BCNQ1_BDNY_V1, fifo->rsvd_boundary);
	rtw_write32(rtwdev, REG_RXFF_BNDY, chip->rxff_size - C2H_PKT_BUF - 1);

	rtw_write8_set(rtwdev, REG_AUTO_LLT_V1, BIT_AUTO_INIT_LLT_V1);

	if (!check_hw_ready(rtwdev, REG_AUTO_LLT_V1, BIT_AUTO_INIT_LLT_V1, 0))
		return -EBUSY;

	rtw_write8(rtwdev, REG_CR + 3, 0);
	return 0;
}

static int priority_queue_cfg(struct rtw_dev *rtwdev)
{
	const struct rtw_chip_info *chip = rtwdev->chip;
	struct rtw_fifo_conf *fifo = &rtwdev->fifo;
	const struct rtw_page_table *pg_tbl = NULL;
	u16 pubq_num;
	int ret;

	ret = rtw_set_trx_fifo_info(rtwdev);
	if (ret)
		return ret;

	switch (rtw_hci_type(rtwdev)) {
	case RTW_HCI_TYPE_PCIE:
		pg_tbl = &chip->page_table[1];
		break;
	default:
		return -EINVAL;
	}

	pubq_num = fifo->acq_pg_num - pg_tbl->hq_num - pg_tbl->lq_num -
		   pg_tbl->nq_num - pg_tbl->exq_num - pg_tbl->gapq_num;
	return __priority_queue_cfg(rtwdev, pg_tbl, pubq_num);
}

static int init_h2c(struct rtw_dev *rtwdev)
{
	struct rtw_fifo_conf *fifo = &rtwdev->fifo;
	u8 value8;
	u32 value32;
	u32 h2cq_addr, h2cq_size, h2cq_free, wp, rp;

	h2cq_addr = fifo->rsvd_h2cq_addr << TX_PAGE_SIZE_SHIFT;
	h2cq_size = RSVD_PG_H2CQ_NUM << TX_PAGE_SIZE_SHIFT;

	value32 = rtw_read32(rtwdev, REG_H2C_HEAD);
	value32 = (value32 & 0xFFFC0000) | h2cq_addr;
	rtw_write32(rtwdev, REG_H2C_HEAD, value32);

	value32 = rtw_read32(rtwdev, REG_H2C_READ_ADDR);
	value32 = (value32 & 0xFFFC0000) | h2cq_addr;
	rtw_write32(rtwdev, REG_H2C_READ_ADDR, value32);

	value32 = rtw_read32(rtwdev, REG_H2C_TAIL);
	value32 &= 0xFFFC0000;
	value32 |= (h2cq_addr + h2cq_size);
	rtw_write32(rtwdev, REG_H2C_TAIL, value32);

	value8 = rtw_read8(rtwdev, REG_H2C_INFO);
	value8 = (u8)((value8 & 0xFC) | 0x01);
	rtw_write8(rtwdev, REG_H2C_INFO, value8);

	value8 = rtw_read8(rtwdev, REG_H2C_INFO);
	value8 = (u8)((value8 & 0xFB) | 0x04);
	rtw_write8(rtwdev, REG_H2C_INFO, value8);

	value8 = rtw_read8(rtwdev, REG_TXDMA_OFFSET_CHK + 1);
	value8 = (u8)((value8 & 0x7f) | 0x80);
	rtw_write8(rtwdev, REG_TXDMA_OFFSET_CHK + 1, value8);

	wp = rtw_read32(rtwdev, REG_H2C_PKT_WRITEADDR) & 0x3FFFF;
	rp = rtw_read32(rtwdev, REG_H2C_PKT_READADDR) & 0x3FFFF;
	h2cq_free = wp >= rp ? h2cq_size - (wp - rp) : rp - wp;

	if (h2cq_size != h2cq_free) {
		rtw_err(rtwdev, "H2C queue mismatch\n");
		return -EINVAL;
	}
	return 0;
}

static int rtw_init_trx_cfg(struct rtw_dev *rtwdev)
{
	int ret = txdma_queue_mapping(rtwdev);
	if (ret)
		return ret;
	ret = priority_queue_cfg(rtwdev);
	if (ret)
		return ret;
	return init_h2c(rtwdev);
}

static int rtw_drv_info_cfg(struct rtw_dev *rtwdev)
{
	u8 value8;

	rtw_write8(rtwdev, REG_RX_DRVINFO_SZ, PHY_STATUS_SIZE);
	if (rtw_chip_wcpu_3081(rtwdev)) {
		value8 = rtw_read8(rtwdev, REG_TRXFF_BNDY + 1);
		value8 &= 0xF0;
		value8 |= 0xF;
		rtw_write8(rtwdev, REG_TRXFF_BNDY + 1, value8);
	}
	rtw_write32_set(rtwdev, REG_RCR, BIT_APP_PHYSTS);
	rtw_write32_clr(rtwdev, REG_WMAC_OPTION_FUNCTION + 4, BIT(8) | BIT(9));
	return 0;
}

/* =================== VERBATIM from upstream rtw8821c.c =================== */

static int rtw8821c_mac_init(struct rtw_dev *rtwdev)
{
	u32 value32;
	u16 pre_txcnt;

	/* protocol configuration */
	rtw_write8(rtwdev, REG_AMPDU_MAX_TIME_V1, WLAN_AMPDU_MAX_TIME);
	rtw_write8_set(rtwdev, REG_TX_HANG_CTRL, BIT_EN_EOF_V1);
	pre_txcnt = WLAN_PRE_TXCNT_TIME_TH | BIT_EN_PRECNT;
	rtw_write8(rtwdev, REG_PRECNT_CTRL, (u8)(pre_txcnt & 0xFF));
	rtw_write8(rtwdev, REG_PRECNT_CTRL + 1, (u8)(pre_txcnt >> 8));
	value32 = WLAN_RTS_LEN_TH | (WLAN_RTS_TX_TIME_TH << 8) |
		  (WLAN_MAX_AGG_PKT_LIMIT << 16) |
		  (WLAN_RTS_MAX_AGG_PKT_LIMIT << 24);
	rtw_write32(rtwdev, REG_PROT_MODE_CTRL, value32);
	rtw_write16(rtwdev, REG_BAR_MODE_CTRL + 2,
		    WLAN_BAR_RETRY_LIMIT | WLAN_RA_TRY_RATE_AGG_LIMIT << 8);
	rtw_write8(rtwdev, REG_FAST_EDCA_VOVI_SETTING, FAST_EDCA_VO_TH);
	rtw_write8(rtwdev, REG_FAST_EDCA_VOVI_SETTING + 2, FAST_EDCA_VI_TH);
	rtw_write8(rtwdev, REG_FAST_EDCA_BEBK_SETTING, FAST_EDCA_BE_TH);
	rtw_write8(rtwdev, REG_FAST_EDCA_BEBK_SETTING + 2, FAST_EDCA_BK_TH);
	rtw_write8_set(rtwdev, REG_INIRTS_RATE_SEL, BIT(5));

	/* EDCA configuration */
	rtw_write8_clr(rtwdev, REG_TIMER0_SRC_SEL, BIT_TSFT_SEL_TIMER0);
	rtw_write16(rtwdev, REG_TXPAUSE, 0);
	rtw_write8(rtwdev, REG_SLOT, WLAN_SLOT_TIME);
	rtw_write8(rtwdev, REG_PIFS, WLAN_PIFS_TIME);
	rtw_write32(rtwdev, REG_SIFS, WLAN_SIFS_CFG);
	rtw_write16(rtwdev, REG_EDCA_VO_PARAM + 2, WLAN_VO_TXOP_LIMIT);
	rtw_write16(rtwdev, REG_EDCA_VI_PARAM + 2, WLAN_VI_TXOP_LIMIT);
	rtw_write32(rtwdev, REG_RD_NAV_NXT, WLAN_NAV_CFG);
	rtw_write16(rtwdev, REG_RXTSF_OFFSET_CCK, WLAN_RX_TSF_CFG);

	/* Set beacon control - enable TSF and other related functions */
	rtw_write8_set(rtwdev, REG_BCN_CTRL, BIT_EN_BCN_FUNCTION);

	/* Set send beacon related registers */
	rtw_write32(rtwdev, REG_TBTT_PROHIBIT, WLAN_TBTT_TIME);
	rtw_write8(rtwdev, REG_DRVERLYINT, WLAN_DRV_EARLY_INT);
	rtw_write8(rtwdev, REG_BCNDMATIM, WLAN_BCN_DMA_TIME);
	rtw_write8_clr(rtwdev, REG_TX_PTCL_CTRL + 1, BIT_SIFS_BK_EN >> 8);

	/* WMAC configuration */
	rtw_write32(rtwdev, REG_RXFLTMAP0, WLAN_RX_FILTER0);
	rtw_write16(rtwdev, REG_RXFLTMAP2, WLAN_RX_FILTER2);
	rtw_write32(rtwdev, REG_RCR, WLAN_RCR_CFG);
	rtw_write8(rtwdev, REG_RX_PKT_LIMIT, WLAN_RXPKT_MAX_SZ_512);
	rtw_write8(rtwdev, REG_TCR + 2, WLAN_TX_FUNC_CFG2);
	rtw_write8(rtwdev, REG_TCR + 1, WLAN_TX_FUNC_CFG1);
	rtw_write8(rtwdev, REG_ACKTO_CCK, 0x40);
	rtw_write8_set(rtwdev, REG_WMAC_TRXPTCL_CTL_H, BIT(1));
	/* Disable the VHT-SIG-B CRC check. Realtek chips default to checking the CRC of
	 * VHT-SIG-B in the 802.11ac signal field, but many APs don't compute it — so their
	 * VHT (5GHz) data frames fail the check and are dropped IN HARDWARE before the
	 * driver sees them, collapsing downlink while uplink (our TX) is unaffected. This
	 * is a prime 5GHz-VHT downlink killer. Address verified vs upstream rtw88 bf.h:
	 *   REG_SND_PTCL_CTRL = 0x0718, BIT_DIS_CHK_VHTSIGB_CRC = BIT(6). */
	rtw_write8_set(rtwdev, 0x0718 /*REG_SND_PTCL_CTRL*/, BIT(6) /*BIT_DIS_CHK_VHTSIGB_CRC*/);
	rtw_write32(rtwdev, REG_WMAC_OPTION_FUNCTION + 8, WLAN_MAC_OPT_FUNC2);
	rtw_write8(rtwdev, REG_WMAC_OPTION_FUNCTION + 4, WLAN_MAC_OPT_NORM_FUNC1);

	return 0;
}

/* ===================== orchestrator (rtw_mac_init) ======================= */

int mac_init(struct rtw_dev *rtwdev)
{
	int ret = rtw_init_trx_cfg(rtwdev);
	if (ret) { rtw_err(rtwdev, "init_trx_cfg failed %d", ret); return ret; }

	ret = rtw8821c_mac_init(rtwdev);
	if (ret) { rtw_err(rtwdev, "chip mac_init failed %d", ret); return ret; }

	ret = rtw_drv_info_cfg(rtwdev);
	if (ret) { rtw_err(rtwdev, "drv_info_cfg failed %d", ret); return ret; }

	/* rtw_hci_interface_cfg is a no-op for 8821c (only 8822C cut>=D acts) */
	return 0;
}
