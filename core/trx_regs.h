/*
 * trx_regs.h — constants for TRX rings + firmware DDMA, copied VERBATIM from
 * upstream rtw88 headers that can't be #included whole here (pci.h/tx.h pull in
 * main.h). Values are upstream's; provenance noted per block. reg.h is already
 * included via rtw_shim.h and supplies REG_ and BIT_ defs (DDMA, MCUFWDL, FW_READY).
 */
#ifndef TRX_REGS_H
#define TRX_REGS_H

#include "rtw_shim.h"

/* __le types as plain integers (LE host) */
typedef u16 __le16;
typedef u32 __le32;

/* little-endian host helpers (macOS x86_64 is LE, so these are identities) */
#define cpu_to_le16(x)  ((u16)(x))
#define cpu_to_le32(x)  ((u32)(x))
#define le16_to_cpu(x)  ((u16)(x))
#define le32_to_cpu(x)  ((u32)(x))
#define u32_encode_bits(v, mask)  ((((u32)(v)) << __builtin_ctz(mask)) & (mask))
#define le32_encode_bits(v, mask) u32_encode_bits(v, mask)

/* shared register poll (upstream mac.c check_hw_ready semantics) */
#define DDMA_POLLING_COUNT_DEF 1000
static inline u32 rtw_read32_mask(u32 addr, u32 mask) { return (hw_read32(addr) & mask) >> __builtin_ctz(mask); }
static inline bool check_hw_ready(struct rtw_dev *rtwdev, u32 addr, u32 mask, u32 target)
{
    (void)rtwdev;
    for (u32 c = 0; c < DDMA_POLLING_COUNT_DEF; c++) {
        if (rtw_read32_mask(addr, mask) == target) return true;
        udelay(10);
    }
    return false;
}

/* ---- pci.h: ring sizes, BD desc addrs, num/idx regs, BCN work ----------- */
#define RTK_DEFAULT_TX_DESC_NUM 128
#define RTK_BEQ_TX_DESC_NUM     256
/* (RX ring depth + slot size live in trx.c: USCAN_RX_DESC_NUM / USCAN_RX_BUF_SIZE) */

#define RTK_PCI_CTRL            0x300
#define BIT_RST_TRXDMA_INTF     BIT(20)
#define BIT_RX_TAG_EN           BIT(15)

#define RTK_PCI_TXBD_DESA_BCNQ  0x308
#define RTK_PCI_TXBD_DESA_H2CQ  0x1320
#define RTK_PCI_TXBD_DESA_MGMTQ 0x310
#define RTK_PCI_TXBD_DESA_BKQ   0x330
#define RTK_PCI_TXBD_DESA_BEQ   0x328
#define RTK_PCI_TXBD_DESA_VIQ   0x320
#define RTK_PCI_TXBD_DESA_VOQ   0x318
#define RTK_PCI_TXBD_DESA_HI0Q  0x340
#define RTK_PCI_RXBD_DESA_MPDUQ 0x338

#define TRX_BD_IDX_MASK         GENMASK(11, 0)

#define RTK_PCI_TXBD_NUM_H2CQ   0x1328
#define RTK_PCI_TXBD_NUM_MGMTQ  0x380
#define RTK_PCI_TXBD_NUM_BKQ    0x38A
#define RTK_PCI_TXBD_NUM_BEQ    0x388
#define RTK_PCI_TXBD_NUM_VIQ    0x386
#define RTK_PCI_TXBD_NUM_VOQ    0x384
#define RTK_PCI_TXBD_NUM_HI0Q   0x38C
#define RTK_PCI_RXBD_NUM_MPDUQ  0x382

#define RTK_PCI_TXBD_IDX_H2CQ   0x132C
#define RTK_PCI_TXBD_IDX_MGMTQ  0x3B0
#define RTK_PCI_TXBD_IDX_BKQ    0x3AC
#define RTK_PCI_TXBD_IDX_BEQ    0x3A8
#define RTK_PCI_TXBD_IDX_VIQ    0x3A4
#define RTK_PCI_TXBD_IDX_VOQ    0x3A0
#define RTK_PCI_TXBD_IDX_HI0Q   0x3B8
#define RTK_PCI_RXBD_IDX_MPDUQ  0x3B4

#define RTK_PCI_TXBD_RWPTR_CLR  0x39C
#define RTK_PCI_TXBD_H2CQ_CSR   0x1330
#define BIT_CLR_H2CQ_HOST_IDX   BIT(16)
#define BIT_CLR_H2CQ_HW_IDX     BIT(8)

#define RTK_PCI_TXBD_OWN_OFFSET 15
#define RTK_PCI_TXBD_BCN_WORK   0x383
#define BIT_PCI_BCNQ_FLAG       BIT(4)

/* TX queue order (main.h) */
enum rtw_tx_queue_type {
    RTW_TX_QUEUE_BK = 0x0, RTW_TX_QUEUE_BE = 0x1, RTW_TX_QUEUE_VI = 0x2,
    RTW_TX_QUEUE_VO = 0x3, RTW_TX_QUEUE_BCN = 0x4, RTW_TX_QUEUE_MGMT = 0x5,
    RTW_TX_QUEUE_HI0 = 0x6, RTW_TX_QUEUE_H2C = 0x7, RTK_MAX_TX_QUEUE_NUM
};

/* ---- tx.h: TX descriptor bitfields + QSEL ------------------------------- */
#define RTW_TX_DESC_W0_TXPKTSIZE GENMASK(15, 0)
#define RTW_TX_DESC_W0_OFFSET    GENMASK(23, 16)
#define RTW_TX_DESC_W0_BMC       BIT(24)
#define RTW_TX_DESC_W0_LS        BIT(26)
#define RTW_TX_DESC_W0_DISQSELSEQ BIT(31)
#define RTW_TX_DESC_W1_MACID     GENMASK(7, 0)
#define RTW_TX_DESC_W1_QSEL      GENMASK(12, 8)
#define RTW_TX_DESC_W1_RATE_ID   GENMASK(20, 16)
#define RTW_TX_DESC_W1_SEC_TYPE  GENMASK(23, 22)
#define RTW_TX_DESC_W1_PKT_OFFSET GENMASK(28, 24)
#define RTW_TX_DESC_W3_USE_RATE  BIT(8)
#define RTW_TX_DESC_W3_DISDATAFB BIT(10)
#define RTW_TX_DESC_W4_DATARATE  GENMASK(6, 0)
#define RTW_TX_DESC_W8_EN_HWSEQ  BIT(15)   /* upstream tx.h:60 — was BIT(31): a wrong bit
                                            * left HW sequence numbering OFF, so every TX
                                            * frame (mgmt/data/EAPOL) carried seq 0. Breaks
                                            * AP de-dup (stalls the 4-way on retransmits) and
                                            * Block-Ack reordering (downlink throughput). */
#define TX_DESC_QSEL_BEACON      16
#define TX_DESC_QSEL_MGMT        18
#define TX_DESC_QSEL_H2C         19
#define DESC_RATE1M              0x00
#define DESC_RATE6M              0x04
#define DESC_RATE54M             0x0b
/* rate-table ids (enum rtw_rate_index, upstream main.h) — picked by band + the AP's
 * granted HT/VHT caps; used in both the RA_INFO H2C and the TX-descriptor RATE_ID. */
#define RTW_RATEID_BGN_40M_1SS   1   /* 2.4GHz CCK+OFDM+HT, 40MHz, 1SS */
#define RTW_RATEID_BGN_20M_1SS   3   /* 2.4GHz CCK+OFDM+HT, 20MHz, 1SS */
#define RTW_RATEID_GN_N1SS       5   /* 5GHz   OFDM+HT,            1SS */
#define RTW_RATEID_BG            6
#define RTW_RATEID_G             7
#define RTW_RATEID_B_20M         8
#define RTW_RATEID_ARFR1_AC_1SS  10  /* 5GHz   OFDM+VHT,           1SS */
#define RTW_RATEID_ARFR2_AC_2G_1SS 11 /* 2.4GHz CCK+OFDM+VHT,      1SS */

/* ---- mac.h: OCP base addrs + DDMA poll count ---------------------------- */
#define DDMA_POLLING_COUNT       1000
#define OCPBASE_TXBUF_88XX       0x18780000
#define OCPBASE_DMEM_88XX        0x00200000

/* ---- fw.h: header layout (only fields we use) --------------------------- */
#define FW_HDR_SIZE              64
#define FW_HDR_CHKSUM_SIZE       8
struct rtw_fw_hdr {
    __le16 signature;
    u8  category;
    u8  function;
    __le16 version;
    u8  subversion;
    u8  subindex;
    __le32 rsvd;
    __le32 feature;
    u8  month; u8 day; u8 hour; u8 min;
    __le16 year;
    __le16 rsvd3;
    u8  mem_usage;
    u8  rsvd4[3];
    __le16 h2c_fmt_ver;
    __le16 rsvd5;
    __le32 dmem_addr;
    __le32 dmem_size;
    __le32 rsvd6;
    __le32 rsvd7;
    __le32 imem_size;
    __le32 emem_size;
    __le32 emem_addr;
    __le32 imem_addr;
};

#endif /* TRX_REGS_H */
