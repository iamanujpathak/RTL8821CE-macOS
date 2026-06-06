/*
 * rtw88_abi.h — the IOUserClient ABI shared by the RTW88Server kext and the
 * userspace control utility (RTW88Client). ONE definition of the selector numbers
 * and the scalar argument layout, so the two sides can never drift.
 *
 * Design note (no-IOMMU platform): this box runs DisableIoMapper=true /
 * npci=0x2000 — there is NO IOMMU. The device bus-masters to RAW physical
 * addresses with nothing to trap a bad one. Therefore the *bridge* owns DMA
 * addressing: userspace gets a handle + a memory mapping (to read/write the
 * buffer contents), and the only way to put a physical address into a device
 * register is kBridgeRegWriteDma, where the *kext* writes the validated paddr.
 * Userspace can never express an arbitrary bus-master target.
 */
#ifndef RTL8821CE_BRIDGE_ABI_H
#define RTL8821CE_BRIDGE_ABI_H

#include <stdint.h>

/* Bump if the selector numbers / arg layout change. PING returns it so the
 * userspace side can refuse a mismatched kext. */
#define RTW_BRIDGE_ABI_VERSION   11  /* v11: dropped the legacy userspace eth-relay nub */
#define RTW_BRIDGE_PING_MAGIC     0x52545742u   /* 'RTWB' */

/* Max concurrent DMA buffers. A DMA handle is also the IOConnectMapMemory
 * memory-type used to map that buffer into userspace. */
#define RTW_BRIDGE_MAX_DMA        16

/* External-method selectors. Keep in sync with sMethods[] in the kext. */
enum {
    kBridgePing       = 0,  /* in:0                              out:4 magic,abi,(vendor<<16|device),barLen */
    kBridgeRegRead    = 1,  /* in:2 offset,width(1|2|4)          out:1 value     -- BAR2 MMIO read   */
    kBridgeRegWrite   = 2,  /* in:3 offset,width,value           out:0           -- BAR2 MMIO write  */
    kBridgeCfgRead    = 3,  /* in:2 offset,width                 out:1 value     -- PCI config read  */
    kBridgeCfgWrite   = 4,  /* in:3 offset,width,value           out:0           -- PCI config write */
    kBridgeDmaAlloc   = 5,  /* in:1 size                         out:3 handle,paddr_lo,paddr_hi      */
    kBridgeDmaFree    = 6,  /* in:1 handle                       out:0                                */
    kBridgeRegWriteDma= 7,  /* in:4 reg_off,handle,buf_off,width out:0  SAFE: kext writes paddr+off  */
    kBridgeSetMacPower= 8,  /* in:1 on(0|1)                      out:0  userspace asserts MAC powered */
    kBridgeIrqEnable  = 9,  /* in:0                              out:0  lazily arm MSI (needs mac on) */
    kBridgeIrqDisable = 10, /* in:0                              out:0                                */
    kBridgeIrqWait    = 11, /* in:1 timeout_ms                   out:1 fired_count_delta             */
    kBridgeIrqStatus  = 12, /* in:2 offset,width                 out:1 value  ERR(kIOReturnNotReady) if mac off */
    kBridgePower      = 13, /* in:2 mem_enable,busmaster_enable  out:0                                */
    /* ---- in-kext DATA PATH (enX + TX/RX done in the kext, no per-pkt syscall) */
    kBridgeDataStart  = 14, /* structIn: rtw_data_cfg            out:0  publish enX + run TX/RX in-kext */
    kBridgeDataStop   = 15, /* in:0                              out:0  stop the data path + remove enX */
    kBridgeDataLink   = 16, /* in:1 up(0|1)                      out:0  set link active/inactive        */
    kBridgeDataStats  = 17, /* in:0  structOut: 21 u64 tx,txDrop,txBytes,rx,rxBytes,rxRetry,rxMcs,rxMaxDepth,ba,rxErr,rxParse,seqGap,seqBack,miss,rxRingFull,rateSum,rateN,rateMax,retryDup,tidMask,bigJump (read+reset; struct b/c scalar methods cap at 16) */
    /* ---- in-kernel control (the kext runs the whole flow; CLI just commands it) */
    kBridgeKInit      = 18, /* in:0  out:1 (ret<<16|CR<<8|poweron)  in-kernel power-on */
    kBridgeKBringup   = 19, /* in:0  out:1 (ok<<16|MCUFW_CTRL)      full in-kernel bring-up */
    kBridgeKScan      = 20, /* in:0  structOut: rtw_scan_result     bring-up + in-kernel scan */
    kBridgeKConnect   = 21, /* structIn: rtw_connect_req  structOut: rtw_connect_result  in-kernel connect */
    kBridgeKDisconnect= 22, /* in:0  out:0  remove enX + free rings */
    kBridgeNumMethods = 23
};

/* in-kernel connect request (CLI -> kext) + result (kext -> CLI). */
struct rtw_connect_req {
    char ssid[33];
    char pass[64];
    uint8_t _pad[3];
};
struct rtw_connect_result {
    uint32_t status;     /* found | associated<<1 | wpa<<2 | data_up<<3 */
    uint8_t  mac[6];     /* the card's MAC — so the CLI can find enX + DHCP it */
    uint8_t  channel;
    uint8_t  _pad;
};

/* scan results, marshalled from scan.c's network list to the CLI. */
#define RTW_SCAN_MAX 64
struct rtw_scan_entry {
    uint8_t  bssid[6];
    uint8_t  channel;
    uint8_t  hidden;
    char     ssid[33];
    uint8_t  _pad[3];
    uint32_t beacons;
};
struct rtw_scan_result {
    uint32_t count;
    struct rtw_scan_entry nets[RTW_SCAN_MAX];
};

/* Config handed to kBridgeDataStart. Userspace did all the control-plane work
 * (power-on, fw, scan, assoc, WPA2 handshake, key install, media-connect) on its
 * rings; it now hands the kext the ring handles + link params and goes idle. The
 * kext takes over the BE TX queue + the RX ring and runs the data path directly:
 * outputPacket() builds the 802.11+CCMP frame and rings the BE doorbell via MMIO;
 * a kernel poll thread drains the RX ring and inputPacket()s — zero per-packet
 * user<->kernel transitions. The kext allocates its OWN per-slot TX buffer pool
 * (the userspace BE ring used a single shared buffer) and rewrites the BE BDs to
 * point at it; it reuses the already-programmed RX data pool + BD ring as-is. */
struct rtw_data_cfg {
    uint8_t  mac[6];            /* our MAC (802.11 addr2 / Ethernet src)        */
    uint8_t  bssid[6];          /* AP BSSID (802.11 addr1, ToDS)                */
    uint8_t  rate;              /* DESC_RATE* TX initial-rate hint              */
    uint8_t  rate_id;           /* RTW_RATEID_* raid the firmware RA runs       */
    uint8_t  _pad[2];
    uint32_t txbd_handle;       /* DMA handle holding the TX BD rings           */
    uint32_t txbd_be_off;       /* byte offset of the BE queue's BDs within it  */
    uint32_t be_len;            /* BE BD ring entry count                       */
    uint32_t bd_sz;             /* tx_buf_desc_sz (16)                          */
    uint32_t desc_sz;           /* tx_pkt_desc_sz (48)                          */
    uint32_t rxdata_handle;     /* DMA handle of the RX data pool               */
    uint32_t rx_nslots;         /* RX slot count                                */
    uint32_t rx_buf_size;       /* RX slot size                                 */
    uint32_t rx_desc_sz;        /* RX descriptor size (24)                      */
};

/* enX TX-ring sizing for the in-kext data path's output queue + per-slot TX pool. */
#define RTW_ETH_SLOT_SIZE   2048
#define RTW_ETH_NSLOTS      128    /* 128 x 2KB = 256KB ring (contiguous, no-IOMMU safe) */

#endif /* RTL8821CE_BRIDGE_ABI_H */
