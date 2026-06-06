/*
 * trx.c — TRX DMA ring setup + the BCN reserved-page TX leaf, over the bridge.
 *
 * Mirrors upstream rtw88 pci.c: per-queue TX buffer-descriptor rings + one RX
 * ring, DESA/NUM registers, RWPTR/H2CQ reset, and the BCN rsvd-page submit used
 * by firmware download (rtw_pci_tx_write_data + write_data_rsvd_page). Only the
 * OS glue (dma_alloc -> bridge, MMIO -> bridge) differs.
 *
 * Bridge DMA budget: one handle holds ALL TX BD rings (each queue's DESA = that
 * handle's paddr + queue offset, armed via the bridge so it writes a validated
 * paddr); separate handles for the TX packet buffer, the RX BD ring, and the RX
 * data pool. Stays within the bridge's 16-handle limit.
 */
#include <string.h>
#include "trx.h"
#include "trx_regs.h"
#include "config.h"
#include "../server/rtw88_abi.h"

/* No IOMMU => RX data pool must be physically contiguous AND every buffer MUST be
 * large enough for the biggest frame the chip can DMA (A-MSDU up to ~11454). The
 * hardware does NOT honour the BD buf_size for oversized frames — it writes the
 * whole frame, so a too-small buffer overflows into adjacent system RAM and
 * corrupts the machine (no IOMMU to trap it). So: full-size buffers (must stay
 * >= any frame the HW may DMA, or it overruns the slot), but more of them — a
 * 32-slot ring is only ~4.5ms of buffering at 11n/ac rates, so a burst during the
 * RX poll's idle gap overflows it and the silent drop collapses TCP. 64 x 11478 =
 * ~735KB contiguous. */
/* Preferred RX ring depth. The pool must be physically contiguous (no IOMMU), and
 * 64 x 11478 = ~735KB (180 pages) can fail to find a contiguous run under memory
 * pressure. trx_init therefore allocates with a fallback (64 -> 48 -> 32 -> 16 -> 8)
 * and records the depth it got in g_rx_n — big ring for data-path throughput when
 * memory allows, small ring (still enough for scan beacons) when fragmented. */
#define USCAN_RX_DESC_NUM  64
#define USCAN_RX_BUF_SIZE  (11454 + 24)

/* per-queue TX ring descriptor */
struct tx_ring {
    u8      *bd;          /* mapped BD array (tx_buf_desc_sz * len) */
    u64      bd_off;      /* offset of this ring's BDs within g_txbd handle */
    u32      len;         /* number of entries */
    u32      wp, rp;
    u32      desa_reg;    /* DESA register for this queue */
    u32      num_reg;     /* NUM register (0 for BCN) */
};

/* one DMA handle backs all TX BD rings; sub-ranges per queue */
static u64   g_txbd_handle, g_txbd_paddr;  static u8 *g_txbd_vaddr;  static u64 g_txbd_size;
/* a reusable TX packet buffer (desc + payload) for the BCN rsvd-page leaf */
static u64   g_pkt_handle,  g_pkt_paddr;    static u8 *g_pkt_vaddr;   static u64 g_pkt_size;
/* RX BD ring + RX data pool */
static u64   g_rxbd_handle, g_rxbd_paddr;   static u8 *g_rxbd_vaddr;
static u64   g_rxdata_handle, g_rxdata_paddr; static u8 *g_rxdata_vaddr;
static int   g_rx_ok;   /* RX ring fully allocated + programmed */
static u32   g_rx_n = USCAN_RX_DESC_NUM;   /* RX ring depth actually allocated (fallback) */

static struct tx_ring g_tx[RTK_MAX_TX_QUEUE_NUM];

static u32 tx_len_for(u8 q)
{
    if (q == RTW_TX_QUEUE_BE)  return RTK_BEQ_TX_DESC_NUM;
    if (q == RTW_TX_QUEUE_BCN) return 1;
    return RTK_DEFAULT_TX_DESC_NUM;
}

static const struct { u8 q; u32 desa, num; } q_regs[] = {
    { RTW_TX_QUEUE_BK,   RTK_PCI_TXBD_DESA_BKQ,   RTK_PCI_TXBD_NUM_BKQ   },
    { RTW_TX_QUEUE_BE,   RTK_PCI_TXBD_DESA_BEQ,   RTK_PCI_TXBD_NUM_BEQ   },
    { RTW_TX_QUEUE_VI,   RTK_PCI_TXBD_DESA_VIQ,   RTK_PCI_TXBD_NUM_VIQ   },
    { RTW_TX_QUEUE_VO,   RTK_PCI_TXBD_DESA_VOQ,   RTK_PCI_TXBD_NUM_VOQ   },
    { RTW_TX_QUEUE_BCN,  RTK_PCI_TXBD_DESA_BCNQ,  0                      },
    { RTW_TX_QUEUE_MGMT, RTK_PCI_TXBD_DESA_MGMTQ, RTK_PCI_TXBD_NUM_MGMTQ },
    { RTW_TX_QUEUE_HI0,  RTK_PCI_TXBD_DESA_HI0Q,  RTK_PCI_TXBD_NUM_HI0Q  },
    { RTW_TX_QUEUE_H2C,  RTK_PCI_TXBD_DESA_H2CQ,  RTK_PCI_TXBD_NUM_H2CQ  },
};

int trx_init(struct rtw_dev *rtwdev)
{
    u32 bd_sz = rtwdev->chip->tx_buf_desc_sz;     /* 16 */
    u32 rx_bd_sz = rtwdev->chip->rx_buf_desc_sz;  /* 8  */

    /* ---- lay out all TX BD rings inside one DMA handle ---- */
    g_txbd_size = 0;
    for (int i = 0; i < (int)(sizeof(q_regs)/sizeof(q_regs[0])); i++) {
        u8 q = q_regs[i].q;
        g_tx[q].len = tx_len_for(q);
        g_tx[q].desa_reg = q_regs[i].desa;
        g_tx[q].num_reg  = q_regs[i].num;
        g_tx[q].bd_off   = g_txbd_size;
        g_tx[q].wp = g_tx[q].rp = 0;
        g_txbd_size += (u64)g_tx[q].len * bd_sz;
        g_txbd_size = (g_txbd_size + 7) & ~7ull;   /* 8-byte align next ring */
    }
    if (hw_dma_alloc(g_txbd_size, &g_txbd_handle, &g_txbd_paddr, (void **)&g_txbd_vaddr))
        { rtw_err(rtwdev, "TX BD alloc failed"); return -1; }
    memset(g_txbd_vaddr, 0, g_txbd_size);
    for (int q = 0; q < RTK_MAX_TX_QUEUE_NUM; q++)
        g_tx[q].bd = g_txbd_vaddr + g_tx[q].bd_off;

    /* ---- TX packet buffer (desc + up to a 4K fw chunk) ---- */
    g_pkt_size = 0x2000;
    if (hw_dma_alloc(g_pkt_size, &g_pkt_handle, &g_pkt_paddr, (void **)&g_pkt_vaddr))
        { rtw_err(rtwdev, "TX pkt alloc failed"); return -1; }

    /* ---- RX ring: BDs + a contiguous data pool (OPTIONAL — fw download needs
     * only TX; don't let a big-contiguous-alloc failure block the fw download) ---- */
    g_rx_ok = 0;
    static const u32 rx_try[] = { USCAN_RX_DESC_NUM, 48, 32, 16, 8 };
    for (unsigned ti = 0; ti < sizeof(rx_try) / sizeof(rx_try[0]) && !g_rx_ok; ti++) {
        u32 n = rx_try[ti];
        if (hw_dma_alloc((u64)n * rx_bd_sz, &g_rxbd_handle, &g_rxbd_paddr,
                         (void **)&g_rxbd_vaddr) != 0)
            continue;                                   /* BD ring is tiny — should always succeed */
        if (hw_dma_alloc((u64)n * USCAN_RX_BUF_SIZE, &g_rxdata_handle, &g_rxdata_paddr,
                         (void **)&g_rxdata_vaddr) != 0) {
            hw_dma_free(g_rxbd_handle, g_rxbd_vaddr, (u64)n * rx_bd_sz);  /* drop, try smaller */
            g_rxbd_vaddr = NULL;
            continue;
        }
        g_rx_n = n;
        memset(g_rxbd_vaddr, 0, (u64)n * rx_bd_sz);
        for (u32 i = 0; i < n; i++) {
            u8 *bd = g_rxbd_vaddr + (u64)i * rx_bd_sz;
            u32 pa = (u32)(g_rxdata_paddr + (u64)i * USCAN_RX_BUF_SIZE);
            *(u16 *)(bd + 0) = cpu_to_le16(USCAN_RX_BUF_SIZE);  /* buf_size */
            *(u16 *)(bd + 2) = 0;                                  /* total_pkt_size */
            *(u32 *)(bd + 4) = cpu_to_le32(pa);                    /* dma */
        }
        g_rx_ok = 1;
        rtw_info(rtwdev, "RX ring: %u descs (%u KB contiguous)", n,
                 (u32)((u64)n * USCAN_RX_BUF_SIZE / 1024));
    }
    if (!g_rx_ok)
        rtw_warn(rtwdev, "RX ring alloc failed (even 8 descs) — continuing TX-only");

    trx_reset(rtwdev);
    return 0;
}

void trx_reset(struct rtw_dev *rtwdev)
{
    u8 tmp = hw_read8(RTK_PCI_CTRL + 3);
    hw_write8(RTK_PCI_CTRL + 3, tmp | 0xf7);

    /* program each TX queue's DESA (ring base) via the bridge's safe arm so the
     * KEXT writes the validated paddr; NUM via plain write. BCN: DESA only. */
    for (int i = 0; i < (int)(sizeof(q_regs)/sizeof(q_regs[0])); i++) {
        u8 q = q_regs[i].q;
        g_tx[q].wp = g_tx[q].rp = 0;
        if (g_tx[q].num_reg)
            hw_write16(g_tx[q].num_reg, g_tx[q].len & 0xfff);
        hw_reg_write_dma(g_tx[q].desa_reg, g_txbd_handle, g_tx[q].bd_off, 4);
    }

    /* RX ring (only if allocated) */
    if (g_rx_ok) {
        hw_write16(RTK_PCI_RXBD_NUM_MPDUQ, g_rx_n & 0xfff);
        hw_reg_write_dma(RTK_PCI_RXBD_DESA_MPDUQ, g_rxbd_handle, 0, 4);
    }

    /* reset read/write pointers + H2C queue index */
    hw_write32(RTK_PCI_TXBD_RWPTR_CLR, 0xffffffff);
    if (rtw_chip_wcpu_3081(rtwdev))
        hw_write32(RTK_PCI_TXBD_H2CQ_CSR,
                   hw_read32(RTK_PCI_TXBD_H2CQ_CSR) | BIT_CLR_H2CQ_HOST_IDX | BIT_CLR_H2CQ_HW_IDX);
    (void)rtwdev;
}

/* BCN reserved-page submit — the firmware-download send leaf.
 * Layout per upstream rtw_pci_tx_write_data(RTW_TX_QUEUE_BCN):
 *   packet buffer = [tx_pkt_desc_sz zeros + filled desc][payload]
 *   BD (16B) seg0 = {buf_size=desc_sz, psb_len|OWN, dma=pkt}
 *           seg1 = {buf_size=size,     -,            dma=pkt+desc_sz}
 *   kick = RTK_PCI_TXBD_BCN_WORK |= BIT_PCI_BCNQ_FLAG
 */
int trx_write_data_rsvd_page(struct rtw_dev *rtwdev, u8 *buf, u32 size)
{
    u32 desc_sz = rtwdev->chip->tx_pkt_desc_sz;   /* 48 */
    u32 total   = size + desc_sz;
    if (total > g_pkt_size) { rtw_err(rtwdev, "rsvd pkt too big"); return -1; }

    /* build packet: zero desc, copy payload after it */
    memset(g_pkt_vaddr, 0, desc_sz);
    memcpy(g_pkt_vaddr + desc_sz, buf, size);

    /* fill TX descriptor for an RSVD_BEACON page (rtw_tx_fill_tx_desc subset) */
    u8 a1 = buf[0];
    int bmc = (a1 & 0x01) ? 1 : 0;
    u32 *w = (u32 *)g_pkt_vaddr;
    w[0] = le32_encode_bits(size, RTW_TX_DESC_W0_TXPKTSIZE) |
           le32_encode_bits(desc_sz, RTW_TX_DESC_W0_OFFSET) |
           le32_encode_bits(bmc, RTW_TX_DESC_W0_BMC) |
           le32_encode_bits(1, RTW_TX_DESC_W0_LS) |
           le32_encode_bits(1, RTW_TX_DESC_W0_DISQSELSEQ);
    w[1] = le32_encode_bits(TX_DESC_QSEL_BEACON, RTW_TX_DESC_W1_QSEL);
    w[8] = le32_encode_bits(1, RTW_TX_DESC_W8_EN_HWSEQ);

    /* BCN ring entry 0 (reused; reset wp) */
    struct tx_ring *ring = &g_tx[RTW_TX_QUEUE_BCN];
    ring->wp = ring->rp = 0;
    u32 psb_len = (total - 1) / 128 + 1;
    psb_len |= 1u << RTK_PCI_TXBD_OWN_OFFSET;

    u8 *bd = ring->bd;                 /* 16-byte BD = two 8-byte segments */
    *(u16 *)(bd + 0) = cpu_to_le16((u16)desc_sz);          /* seg0 buf_size */
    *(u16 *)(bd + 2) = cpu_to_le16((u16)psb_len);          /* seg0 psb_len|OWN */
    *(u32 *)(bd + 4) = cpu_to_le32((u32)g_pkt_paddr);      /* seg0 dma */
    *(u16 *)(bd + 8) = cpu_to_le16((u16)size);             /* seg1 buf_size */
    *(u16 *)(bd + 10) = 0;
    *(u32 *)(bd + 12) = cpu_to_le32((u32)(g_pkt_paddr + desc_sz)); /* seg1 dma */

    /* kick the beacon queue */
    u8 work = hw_read8(RTK_PCI_TXBD_BCN_WORK);
    hw_write8(RTK_PCI_TXBD_BCN_WORK, work | (u8)BIT_PCI_BCNQ_FLAG);
    return 0;
}

/* Send a management frame through the MGMT queue (the first real 802.11 TX).
 * Layout mirrors rtw_pci_tx_write_data for a non-BCN queue: [desc][frame] in a
 * DMA buffer, a 2-segment BD, then bump wp and ring the MGMT doorbell. */
int trx_tx_mgmt(struct rtw_dev *rtwdev, const u8 *frame, u32 len, u8 rate, int bmc)
{
    u32 desc_sz = rtwdev->chip->tx_pkt_desc_sz;   /* 48 */
    u32 total   = len + desc_sz;
    if (total > g_pkt_size) { rtw_err(rtwdev, "mgmt frame too big"); return -1; }

    memset(g_pkt_vaddr, 0, desc_sz);
    memcpy(g_pkt_vaddr + desc_sz, frame, len);

    /* rtw_tx_mgmt_pkt_info_update: dis_qselseq + en_hwseq + rate_id + use_rate */
    u8 rate_id = (rate == DESC_RATE1M) ? RTW_RATEID_B_20M : RTW_RATEID_G;
    u32 *w = (u32 *)g_pkt_vaddr;
    w[0] = le32_encode_bits(len, RTW_TX_DESC_W0_TXPKTSIZE) |
           le32_encode_bits(desc_sz, RTW_TX_DESC_W0_OFFSET) |
           le32_encode_bits(bmc ? 1 : 0, RTW_TX_DESC_W0_BMC) |
           le32_encode_bits(1, RTW_TX_DESC_W0_LS) |
           le32_encode_bits(1, RTW_TX_DESC_W0_DISQSELSEQ);
    w[1] = le32_encode_bits(TX_DESC_QSEL_MGMT, RTW_TX_DESC_W1_QSEL) |
           le32_encode_bits(rate_id, RTW_TX_DESC_W1_RATE_ID);
    w[3] = le32_encode_bits(1, RTW_TX_DESC_W3_USE_RATE) |
           le32_encode_bits(1, RTW_TX_DESC_W3_DISDATAFB);
    w[4] = le32_encode_bits(rate, RTW_TX_DESC_W4_DATARATE);
    w[8] = le32_encode_bits(1, RTW_TX_DESC_W8_EN_HWSEQ);

    struct tx_ring *ring = &g_tx[RTW_TX_QUEUE_MGMT];
    u8 *bd = ring->bd + (u64)ring->wp * 16;       /* tx_buf_desc_sz = 16 */
    u32 psb_len = (total - 1) / 128 + 1;          /* no OWN bit for non-BCN */
    *(u16 *)(bd + 0) = cpu_to_le16((u16)desc_sz);
    *(u16 *)(bd + 2) = cpu_to_le16((u16)psb_len);
    *(u32 *)(bd + 4) = cpu_to_le32((u32)g_pkt_paddr);
    *(u16 *)(bd + 8) = cpu_to_le16((u16)len);
    *(u16 *)(bd + 10) = 0;
    *(u32 *)(bd + 12) = cpu_to_le32((u32)(g_pkt_paddr + desc_sz));

    ring->wp = (ring->wp + 1) % ring->len;
    hw_write16(RTK_PCI_TXBD_IDX_MGMTQ, (u16)(ring->wp & 0xfff));   /* doorbell */

    /* one-shot diagnostic: did the chip dequeue the BD? (hw idx should reach wp) */
    static int diag = 0;
    if (!diag) {
        diag = 1;
        usleep(3000);
        u32 idx = hw_read32(RTK_PCI_TXBD_IDX_MGMTQ);
        printf("  [TXdiag] MGMT IDX host=%u hw=%u | CR=0x%02x TXPAUSE=0x%04x cnt=0x%08x\n",
               idx & 0xfff, (idx >> 16) & 0xfff, hw_read8(REG_CR),
               hw_read16(REG_TXPAUSE), hw_read32(0x0664) /* REG_TX OK cnt-ish */);
    }
    return 0;
}

/* Send an 802.11 DATA frame via the BE queue (for EAPOL / data). sec=0 means no
 * hardware encryption (the 4-way handshake frames go out in the clear).
 *
 * CCMP note (sec_type==3): upstream rtw88 sets IEEE80211_KEY_FLAG_GENERATE_IV, so
 * the *driver* supplies the 8-byte CCMP header (IV + 48-bit PN) in the frame and
 * the hardware only encrypts the payload + appends the 8-byte MIC. We therefore
 * insert an incrementing-PN CCMP header right after the 802.11 MAC header; the
 * over-the-air size grows by 8 (IV). The hardware adds the MIC, not counted here. */
int trx_tx_data(struct rtw_dev *rtwdev, const u8 *frame, u32 len, u8 rate, u8 qsel, u8 sec_type)
{
    static u64 g_ccmp_pn = 1;                     /* TX packet number, must increase */
    u32 desc_sz = rtwdev->chip->tx_pkt_desc_sz;   /* 48 */
    u8 *body = g_pkt_vaddr + desc_sz;
    u32 air_len = len;                            /* bytes the chip reads after desc */

    if (sec_type == 3) {
        /* header length: 24, +2 for QoS data (subtype bit 7 of the data type) */
        u16 fc = (u16)(frame[0] | (frame[1] << 8));
        u32 hdr_len = 24 + (((fc & 0x0c) == 0x08 && (fc & 0x80)) ? 2 : 0);
        u64 pn = g_ccmp_pn++;
        u8 keyid = 0;
        if (desc_sz + len + 8 > g_pkt_size) { rtw_err(rtwdev, "data frame too big"); return -1; }
        memset(g_pkt_vaddr, 0, desc_sz);
        memcpy(body, frame, hdr_len);             /* 802.11 header */
        u8 *iv = body + hdr_len;                  /* 8-byte CCMP header */
        iv[0] = (u8)(pn);          iv[1] = (u8)(pn >> 8);
        iv[2] = 0;                 iv[3] = 0x20 | (keyid << 6);   /* ExtIV bit set */
        iv[4] = (u8)(pn >> 16);    iv[5] = (u8)(pn >> 24);
        iv[6] = (u8)(pn >> 32);    iv[7] = (u8)(pn >> 40);
        memcpy(iv + 8, frame + hdr_len, len - hdr_len);  /* payload */
        air_len = len + 8;
    } else {
        if (desc_sz + len > g_pkt_size) { rtw_err(rtwdev, "data frame too big"); return -1; }
        memset(g_pkt_vaddr, 0, desc_sz);
        memcpy(body, frame, len);
    }
    u32 total = air_len + desc_sz;

    /* Encrypted bulk data (CCMP) -> let the firmware rate-adaptation pick the rate
     * (USE_RATE/DISDATAFB cleared, MACID+RATE_ID set so the fw uses the per-peer RA
     * table programmed by fw_ra_info). `rate` becomes only the initial hint. EAPOL
     * and in-clear frames (sec_type 0) stay pinned for handshake reliability. */
    int use_ra = (sec_type == 3);
    u8  rate_id = g_session.raid;   /* the HT/VHT/legacy raid the firmware RA runs for MACID 0 */
    u32 *w = (u32 *)g_pkt_vaddr;
    w[0] = le32_encode_bits(air_len, RTW_TX_DESC_W0_TXPKTSIZE) |
           le32_encode_bits(desc_sz, RTW_TX_DESC_W0_OFFSET) |
           le32_encode_bits(1, RTW_TX_DESC_W0_LS);
    w[1] = le32_encode_bits(qsel, RTW_TX_DESC_W1_QSEL) |
           le32_encode_bits(sec_type, RTW_TX_DESC_W1_SEC_TYPE) |
           le32_encode_bits(use_ra ? rate_id : 0, RTW_TX_DESC_W1_RATE_ID);  /* MACID 0 implicit */
    if (!use_ra)
        w[3] = le32_encode_bits(1, RTW_TX_DESC_W3_USE_RATE) |
               le32_encode_bits(1, RTW_TX_DESC_W3_DISDATAFB);
    w[4] = le32_encode_bits(rate, RTW_TX_DESC_W4_DATARATE);          /* init hint */
    w[8] = le32_encode_bits(1, RTW_TX_DESC_W8_EN_HWSEQ);

    struct tx_ring *ring = &g_tx[RTW_TX_QUEUE_BE];
    u8 *bd = ring->bd + (u64)ring->wp * 16;
    u32 psb_len = (total - 1) / 128 + 1;
    *(u16 *)(bd + 0) = cpu_to_le16((u16)desc_sz);
    *(u16 *)(bd + 2) = cpu_to_le16((u16)psb_len);
    *(u32 *)(bd + 4) = cpu_to_le32((u32)g_pkt_paddr);
    *(u16 *)(bd + 8) = cpu_to_le16((u16)air_len);
    *(u16 *)(bd + 10) = 0;
    *(u32 *)(bd + 12) = cpu_to_le32((u32)(g_pkt_paddr + desc_sz));

    ring->wp = (ring->wp + 1) % ring->len;
    hw_write16(RTK_PCI_TXBD_IDX_BEQ, (u16)(ring->wp & 0xfff));   /* BE doorbell */
    return 0;
}

/* Send a 32-byte H2C packet to the firmware via the H2C queue (no rate; it goes
 * to the MCU, not over the air). Same BD/doorbell shape as MGMT, queue = H2C. */
int trx_tx_h2c(struct rtw_dev *rtwdev, const u8 *pkt)
{
    u32 desc_sz = rtwdev->chip->tx_pkt_desc_sz;   /* 48 */
    u32 len = 32;                                  /* H2C_PKT_SIZE */
    u32 total = len + desc_sz;
    if (total > g_pkt_size) return -1;

    memset(g_pkt_vaddr, 0, desc_sz);
    memcpy(g_pkt_vaddr + desc_sz, pkt, len);

    u32 *w = (u32 *)g_pkt_vaddr;
    w[0] = le32_encode_bits(len, RTW_TX_DESC_W0_TXPKTSIZE) |
           le32_encode_bits(desc_sz, RTW_TX_DESC_W0_OFFSET);
    w[1] = le32_encode_bits(TX_DESC_QSEL_H2C, RTW_TX_DESC_W1_QSEL);

    struct tx_ring *ring = &g_tx[RTW_TX_QUEUE_H2C];
    u8 *bd = ring->bd + (u64)ring->wp * 16;
    u32 psb_len = (total - 1) / 128 + 1;
    *(u16 *)(bd + 0) = cpu_to_le16((u16)desc_sz);
    *(u16 *)(bd + 2) = cpu_to_le16((u16)psb_len);
    *(u32 *)(bd + 4) = cpu_to_le32((u32)g_pkt_paddr);
    *(u16 *)(bd + 8) = cpu_to_le16((u16)len);
    *(u16 *)(bd + 10) = 0;
    *(u32 *)(bd + 12) = cpu_to_le32((u32)(g_pkt_paddr + desc_sz));

    ring->wp = (ring->wp + 1) % ring->len;
    hw_write16(RTK_PCI_TXBD_IDX_H2CQ, (u16)(ring->wp & 0xfff));   /* H2C doorbell */
    return 0;
}

/* ---- RX ring access for the scan stage ---------------------------------- */
int  trx_rx_ok(void)        { return g_rx_ok; }
u32  trx_rx_count(void)     { return g_rx_n; }
u32  trx_rx_buf_size(void)  { return USCAN_RX_BUF_SIZE; }
u32  trx_rx_hw_idx(void)    { return (hw_read32(RTK_PCI_RXBD_IDX_MPDUQ) >> 16) & 0xfff; }
u8  *trx_rx_slot_buf(u32 slot)
{ return g_rxdata_vaddr ? g_rxdata_vaddr + (u64)slot * USCAN_RX_BUF_SIZE : NULL; }
void trx_rx_set_host_idx(struct rtw_dev *rtwdev, u32 rp)
{ (void)rtwdev; hw_write16(RTK_PCI_RXBD_IDX_MPDUQ, (u16)(rp & 0xfff)); }

/* fill the ring config the kext data path needs (handles + layout). The caller
 * fills mac/bssid/rate. Hands the kext the BE BD ring + RX data pool to take over. */
void trx_fill_data_cfg(struct rtw_dev *rtwdev, struct rtw_data_cfg *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->txbd_handle = (u32)g_txbd_handle;
    cfg->txbd_be_off = (u32)g_tx[RTW_TX_QUEUE_BE].bd_off;
    cfg->be_len      = g_tx[RTW_TX_QUEUE_BE].len;
    cfg->bd_sz       = rtwdev->chip->tx_buf_desc_sz;   /* 16 */
    cfg->desc_sz     = rtwdev->chip->tx_pkt_desc_sz;   /* 48 */
    cfg->rxdata_handle = (u32)g_rxdata_handle;
    cfg->rx_nslots   = g_rx_n;
    cfg->rx_buf_size = USCAN_RX_BUF_SIZE;
    cfg->rx_desc_sz  = 24;                              /* RX_PKT_DESC_SZ */
}

void trx_free(void)
{
    /* CRITICAL (no IOMMU): stop the chip bus-mastering BEFORE we free the DMA
     * buffers. Otherwise the chip keeps DMA-ing received frames into physical
     * memory the kernel has reused (GPU/framebuffer) -> corruption + freeze,
     * continuing even after this process exits. Halt RX/TX DMA, drop the ring
     * base registers, disable PCI bus mastering, then let in-flight DMA settle. */
    hw_write16(RTK_PCI_RXBD_NUM_MPDUQ, 0);     /* RX ring length 0 */
    hw_write32(RTK_PCI_RXBD_DESA_MPDUQ, 0);    /* clear RX ring base */
    hw_write8(REG_CR, 0);                      /* stop MAC TRX DMA engines */
    hw_power(true, false);                     /* disable PCI bus mastering */
    usleep(30000);                             /* let any in-flight DMA drain */

    if (g_txbd_vaddr)   hw_dma_free(g_txbd_handle, g_txbd_vaddr, g_txbd_size);
    if (g_pkt_vaddr)    hw_dma_free(g_pkt_handle, g_pkt_vaddr, g_pkt_size);
    if (g_rxbd_vaddr)   hw_dma_free(g_rxbd_handle, g_rxbd_vaddr, (u64)g_rx_n * 8);
    if (g_rxdata_vaddr) hw_dma_free(g_rxdata_handle, g_rxdata_vaddr, (u64)g_rx_n * USCAN_RX_BUF_SIZE);
    g_txbd_vaddr = g_pkt_vaddr = g_rxbd_vaddr = g_rxdata_vaddr = NULL;
}
