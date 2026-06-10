/*
 * trx.h — PCI TRX DMA ring setup for the RTL8821CE, over the bridge.
 *
 * Faithful to upstream rtw88 pci.c (BD formats, DESA/NUM/IDX register layout,
 * ring sizes, init sequence) but the DMA allocation + MMIO route through the
 * bridge. No-IOMMU note: ring/buffer physical addresses come from the bridge
 * (real paddrs); DESA registers are programmed via the bridge's safe arm so the
 * kext writes the validated ring base.
 */
#ifndef TRX_H
#define TRX_H

#include "rtw_shim.h"

/* set up all TX queue BD rings + the RX BD ring and program the chip. 0 on ok. */
int  trx_init(struct rtw_dev *rtwdev);
void trx_free(void);
/* clear the allocate-once bookkeeping after the kext freed the DMA buffers (stop()) */
void trx_unload_reset(void);

/* re-program DESA/NUM/RWPTR (upstream rtw_pci_reset_trx_ring). */
void trx_reset(struct rtw_dev *rtwdev);

/* fill the ring config the kext in-kernel data path needs (handles + layout) */
struct rtw_data_cfg;
void trx_fill_data_cfg(struct rtw_dev *rtwdev, struct rtw_data_cfg *cfg);

/* push one reserved-page packet through the BCN queue (the firmware-download
 * leaf). buf/size = raw payload; a tx descriptor is prepended internally. */
int  trx_write_data_rsvd_page(struct rtw_dev *rtwdev, u8 *buf, u32 size);

/* send a management frame through the MGMT queue (first real 802.11 TX) */
int  trx_tx_mgmt(struct rtw_dev *rtwdev, const u8 *frame, u32 len, u8 rate, int bmc);
/* send a 32-byte H2C packet to the firmware via the H2C queue */
int  trx_tx_h2c(struct rtw_dev *rtwdev, const u8 *pkt);
/* send an in-clear 802.11 data frame via the BE queue (the EAPOL handshake) */
int  trx_tx_data(struct rtw_dev *rtwdev, const u8 *frame, u32 len, u8 rate);

/* ---- RX ring access ----------------------------------------------------- */
int  trx_rx_ok(void);            /* RX ring allocated + programmed */
u32  trx_rx_count(void);         /* number of RX descriptors */
u32  trx_rx_buf_size(void);      /* per-buffer size */
u32  trx_rx_hw_idx(void);        /* chip's RX write index (bits 27:16 of RXBD_IDX) */
u8  *trx_rx_slot_buf(u32 slot);  /* mapped RX buffer for a slot */
void trx_rx_set_host_idx(struct rtw_dev *rtwdev, u32 rp);  /* free buffers up to rp */

#endif /* TRX_H */
