/*
 * rtw_shim.h — minimal Linux-compat layer so VERBATIM upstream rtw88 bring-up
 * code (lifted into pwr.c etc.) compiles and runs in userspace, with every
 * register access routed to the bridge HW layer (rtw_hw.h).
 *
 * This deliberately defines only what the lifted routines touch — NOT upstream's
 * full struct rtw_dev. The point is to reuse upstream LOGIC unmodified while
 * keeping the harness small and the hardware path honest.
 */
#ifndef RTW_SHIM_H
#define RTW_SHIM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#if defined(KERNEL) || defined(__KERNEL__)
#include <string.h>          /* mem* in-kernel */
#include <sys/errno.h>       /* EINVAL / EBUSY / ETIMEDOUT */
#include <IOKit/IOLib.h>     /* IOLog / IODelay / IOSleep / IOMalloc */
#else
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#endif

#include "rtw_hw.h"

/* ---- kernel libc compat: lets the proven control C (which uses printf/usleep/
 * malloc/free) compile + run in-kernel. malloc/free carry the size in a prefix
 * word so IOFree gets the length it requires. Defined after all includes so it
 * can't clobber a header declaration. ------------------------------------------ */
#if defined(KERNEL) || defined(__KERNEL__)
static inline void usleep(unsigned us) { if (us >= 1000) IOSleep((us + 999) / 1000); else IODelay(us ? us : 1); }
static inline void *rtw_kmalloc(size_t n) { size_t *p = (size_t *)IOMalloc(n + sizeof(size_t)); if (!p) return (void *)0; *p = n; return p + 1; }
static inline void  rtw_kfree(void *q) { if (!q) return; size_t *p = ((size_t *)q) - 1; IOFree(p, *p + sizeof(size_t)); }
#define printf(...)  IOLog(__VA_ARGS__)
#define malloc(n)    rtw_kmalloc((size_t)(n))
#define free(p)      rtw_kfree(p)
extern void read_random(void *buffer, unsigned int numBytes);   /* xnu CSPRNG */
static inline void arc4random_buf(void *b, size_t n) { read_random(b, (unsigned int)n); }  /* WPA SNonce */
#endif

/* ---- kernel scalar types ------------------------------------------------- */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;

/* ---- bit helpers (upstream reg.h needs BIT/GENMASK) ---------------------- */
#ifndef BIT
#define BIT(n)            (1UL << (n))
#endif
#define GENMASK(h, l)     (((~0UL) << (l)) & (~0UL >> (sizeof(long) * 8 - 1 - (h))))

/* ---- logging ------------------------------------------------------------- */
#if defined(KERNEL) || defined(__KERNEL__)
#define rtw_err(dev, fmt, ...)       IOLog("[rtw err]  " fmt "\n", ##__VA_ARGS__)
#define rtw_warn(dev, fmt, ...)      IOLog("[rtw warn] " fmt "\n", ##__VA_ARGS__)
#define rtw_dbg(dev, mask, fmt, ...) IOLog("[rtw dbg]  " fmt "\n", ##__VA_ARGS__)
#define rtw_info(dev, fmt, ...)      IOLog("[rtw info] " fmt "\n", ##__VA_ARGS__)
#else
#define rtw_err(dev, fmt, ...)   fprintf(stderr, "[rtw err]  " fmt "\n", ##__VA_ARGS__)
#define rtw_warn(dev, fmt, ...)  fprintf(stderr, "[rtw warn] " fmt "\n", ##__VA_ARGS__)
#define rtw_dbg(dev, mask, fmt, ...) fprintf(stderr, "[rtw dbg]  " fmt "\n", ##__VA_ARGS__)
#define rtw_info(dev, fmt, ...)  fprintf(stderr, "[rtw info] " fmt "\n", ##__VA_ARGS__)
#endif
#define EXPORT_SYMBOL(x)

/* ---- delays -------------------------------------------------------------- */
#if defined(KERNEL) || defined(__KERNEL__)
static inline void udelay(unsigned us) { IODelay(us ? us : 1); }            /* busy-wait us */
static inline void mdelay(unsigned ms) { IOSleep(ms ? ms : 1); }           /* sleep ms (worker ctx) */
static inline void msleep(unsigned ms) { IOSleep(ms ? ms : 1); }
static inline void usleep_range(unsigned lo, unsigned hi) { (void)hi; IODelay(lo ? lo : 1); }
#else
static inline void udelay(unsigned us) { usleep(us ? us : 1); }
static inline void mdelay(unsigned ms) { usleep((useconds_t)ms * 1000); }
static inline void msleep(unsigned ms) { usleep((useconds_t)ms * 1000); }
static inline void usleep_range(unsigned lo, unsigned hi) { (void)hi; usleep(lo ? lo : 1); }
#endif

/* ---- bitmap (rtwdev->flags is unsigned long flags[1]) -------------------- */
#define set_bit(b, addr)    (*(addr) |=  (1UL << (b)))
#define clear_bit(b, addr)  (*(addr) &= ~(1UL << (b)))
#define test_bit(b, addr)   ((*(addr) >> (b)) & 1UL)

/* ---- poll helper (faithful to linux/iopoll.h semantics) ------------------ *
 * Repeatedly evaluate `op(args)` into `val` until `cond`, sleeping delay_us
 * between tries, giving up after ~timeout_us. Returns 0 on success, -ETIMEDOUT. */
#define read_poll_timeout_atomic(op, val, cond, delay_us, timeout_us, before, ...) \
({                                                                                 \
    int __ret = 0; unsigned long __elapsed = 0;                                    \
    unsigned long __d = (delay_us) ? (unsigned long)(delay_us) : 1;                \
    for (;;) {                                                                     \
        (val) = op(__VA_ARGS__);                                                   \
        if (cond) break;                                                           \
        if ((timeout_us) && __elapsed >= (unsigned long)(timeout_us)) {            \
            __ret = -ETIMEDOUT; break;                                             \
        }                                                                          \
        udelay(__d); __elapsed += __d;                                             \
    }                                                                              \
    __ret;                                                                         \
})

/* ---- enums upstream code references -------------------------------------- */
enum rtw_hci_type {
    RTW_HCI_TYPE_PCIE,
    RTW_HCI_TYPE_USB,
    RTW_HCI_TYPE_SDIO,
    RTW_HCI_TYPE_UNDEFINE,
};
enum rtw_wlan_cpu { RTW_WCPU_3081, RTW_WCPU_8051 };
enum rtw_chip_type {
    RTW_CHIP_TYPE_8822B, RTW_CHIP_TYPE_8822C, RTW_CHIP_TYPE_8723D,
    RTW_CHIP_TYPE_8821C, RTW_CHIP_TYPE_8703B, RTW_CHIP_TYPE_8821A,
    RTW_CHIP_TYPE_8812A, RTW_CHIP_TYPE_8814A,
};
enum { RTW_FLAG_POWERON, RTW_FLAG_FW_RUNNING };   /* bit indices into flags[] */

/* power-seq command encoding (from upstream main.h) */
#define RTW_PWR_POLLING_CNT  20000
#define RTW_PWR_CMD_READ     0x00
#define RTW_PWR_CMD_WRITE    0x01
#define RTW_PWR_CMD_POLLING  0x02
#define RTW_PWR_CMD_DELAY    0x03
#define RTW_PWR_CMD_END      0x04
#define RTW_PWR_ADDR_MAC     0x00
#define RTW_PWR_ADDR_USB     0x01
#define RTW_PWR_ADDR_PCIE    0x02
#define RTW_PWR_ADDR_SDIO    0x03
#define RTW_PWR_INTF_SDIO_MSK BIT(0)
#define RTW_PWR_INTF_USB_MSK  BIT(1)
#define RTW_PWR_INTF_PCI_MSK  BIT(2)
#define RTW_PWR_INTF_ALL_MSK  (BIT(0) | BIT(1) | BIT(2) | BIT(3))
#define RTW_PWR_CUT_TEST_MSK  BIT(0)
#define RTW_PWR_CUT_A_MSK     BIT(1)
#define RTW_PWR_CUT_ALL_MSK   0xFF
enum { RTW_PWR_DELAY_US, RTW_PWR_DELAY_MS };

/* upstream main.h: struct rtw_pwr_seq_cmd (exact layout) */
struct rtw_pwr_seq_cmd {
    u16 offset;
    u8 cut_mask;
    u8 intf_mask;
    u8 base:4;
    u8 cmd:4;
    u8 mask;
    u8 value;
};

/* SDIO-only constant referenced by the parser (never hit on PCIe) */
#define SDIO_LOCAL_OFFSET  0x10250000

/* ---- TRX fifo / page-table / rqpn (mac_init) ---------------------------- */
enum rtw_dma_mapping {
    RTW_DMA_MAPPING_EXTRA = 0, RTW_DMA_MAPPING_LOW = 1,
    RTW_DMA_MAPPING_NORMAL = 2, RTW_DMA_MAPPING_HIGH = 3,
    RTW_DMA_MAPPING_MAX, RTW_DMA_MAPPING_UNDEF,
};
struct rtw_page_table { u16 hq_num, nq_num, lq_num, exq_num, gapq_num; };
struct rtw_rqpn {
    enum rtw_dma_mapping dma_map_vo, dma_map_vi, dma_map_be,
                         dma_map_bk, dma_map_mg, dma_map_hi;
};
struct rtw_fifo_conf {
    u16 rsvd_drv_pg_num, txff_pg_num, rsvd_pg_num, acq_pg_num, rsvd_boundary;
    u16 rsvd_csibuf_addr, rsvd_fw_txbuf_addr, rsvd_cpu_instr_addr;
    u16 rsvd_h2cq_addr, rsvd_h2c_sta_info_addr, rsvd_h2c_info_addr, rsvd_drv_addr;
    const struct rtw_rqpn *rqpn;
};

/* ---- minimal rtw_dev / chip / hci / hal (only fields the lifted code uses) */
struct rtw_chip_info {
    enum rtw_chip_type id;
    enum rtw_wlan_cpu  wlan_cpu;
    u8 sys_func_en;
    const struct rtw_pwr_seq_cmd * const *pwr_on_seq;
    const struct rtw_pwr_seq_cmd * const *pwr_off_seq;
    /* TRX descriptor sizes (rtw8821c.c chip_info) */
    u32 tx_pkt_desc_sz;
    u32 tx_buf_desc_sz;
    u32 rx_pkt_desc_sz;
    u32 rx_buf_desc_sz;
    u32 ltecoex_addr;   /* 0 = skip ltecoex backup in our harness */
    /* TRX fifo config (mac_init) */
    u32 txff_size;
    u32 rxff_size;
    u8  rsvd_drv_pg_num;
    u8  csi_buf_pg_num;
    u16 page_size;
    const struct rtw_page_table *page_table;
    const struct rtw_rqpn *rqpn_table;
    /* EFUSE sizes (rtw8821c.c chip_info) */
    u32 phy_efuse_size;
    u32 log_efuse_size;
    u32 ptct_efuse_size;
};
/* ---- phy table conditional-eval structs (main.h / phy.h, verbatim LE) ---- */
enum rtw_rf_path { RF_PATH_A = 0, RF_PATH_B = 1 };
enum rtw_bandwidth {
    RTW_CHANNEL_WIDTH_20 = 0, RTW_CHANNEL_WIDTH_40 = 1, RTW_CHANNEL_WIDTH_80 = 2,
    RTW_CHANNEL_WIDTH_160 = 3, RTW_CHANNEL_WIDTH_80_80 = 4,
    RTW_CHANNEL_WIDTH_5 = 5, RTW_CHANNEL_WIDTH_10 = 6,
};
#define INTF_PCIE   BIT(0)
#define INTF_USB    BIT(1)
#define INTF_SDIO   BIT(2)
#define BRANCH_IF    0
#define BRANCH_ELIF  1
#define BRANCH_ELSE  2
#define BRANCH_ENDIF 3
#define RFREG_MASK   0xfffff
struct phy_cfg_pair { u32 addr; u32 data; };
struct rtw_phy_cond {
    u32 rfe:8;
    u32 intf:4;
    u32 pkg:4;
    u32 plat:4;
    u32 intf_rsvd:4;
    u32 cut:4;
    u32 branch:2;
    u32 neg:1;
    u32 pos:1;
};
struct rtw_phy_cond2 { u8 type_glna, type_gpa, type_alna, type_apa; };
union phy_table_tile {
    struct rtw_phy_cond  cond;
    struct rtw_phy_cond2 cond2;
    struct phy_cfg_pair  cfg;
};

struct rtw_hci_min { enum rtw_hci_type type; u16 rpwm_addr; };
struct rtw_hal_min {
    u8  cut_version; u8 pkg_type; bool rfe_btg; u8 rf_path_num; u8 rf_phy_num;
    u32 chip_version;
    struct rtw_phy_cond  phy_cond;
    struct rtw_phy_cond2 phy_cond2;
    u32 ch_param[3];
};
/* fields the phy/rf stages read from efuse */
struct rtw_efuse {
    u16 rtl_id;          /* logical efuse offset 0x00 (decode-alignment check) */
    u8 addr[6];
    u8 rfe_option;
    u8 crystal_cap;
    u8 rf_board_option;
    u8 channel_plan;
    bool btcoex;
};
struct rtw_dev {
    const struct rtw_chip_info *chip;
    struct rtw_hci_min hci;
    struct rtw_hal_min hal;
    struct rtw_fifo_conf fifo;
    struct rtw_efuse efuse;
    unsigned long flags[1];
};

/* helper predicates (upstream inlines) */
static inline enum rtw_hci_type rtw_hci_type(struct rtw_dev *d) { return d->hci.type; }
static inline bool rtw_chip_wcpu_8051(struct rtw_dev *d) { return d->chip->wlan_cpu == RTW_WCPU_8051; }
static inline bool rtw_chip_wcpu_3081(struct rtw_dev *d) { return d->chip->wlan_cpu == RTW_WCPU_3081; }
static inline bool rtw_sdio_is_sdio30_supported(struct rtw_dev *d) { (void)d; return false; }
#define cut_version_to_mask(cut)  (0x1 << ((cut) + 1))

/* ---- register access: route upstream rtw_read / rtw_write to the bridge --- *
 * The single device is implicit, so the rtwdev arg is ignored. */
#define rtw_read8(dev, addr)        hw_read8((u32)(addr))
#define rtw_read16(dev, addr)       hw_read16((u32)(addr))
#define rtw_read32(dev, addr)       hw_read32((u32)(addr))
#define rtw_write8(dev, addr, v)    hw_write8((u32)(addr), (u8)(v))
#define rtw_write16(dev, addr, v)   hw_write16((u32)(addr), (u16)(v))
#define rtw_write32(dev, addr, v)   hw_write32((u32)(addr), (u32)(v))

#define rtw_write8_set(dev, addr, bits)  hw_write8 ((u32)(addr), hw_read8 ((u32)(addr)) |  (u8)(bits))
#define rtw_write8_clr(dev, addr, bits)  hw_write8 ((u32)(addr), hw_read8 ((u32)(addr)) & ~(u8)(bits))
#define rtw_write16_set(dev, addr, bits) hw_write16((u32)(addr), hw_read16((u32)(addr)) |  (u16)(bits))
#define rtw_write16_clr(dev, addr, bits) hw_write16((u32)(addr), hw_read16((u32)(addr)) & ~(u16)(bits))
#define rtw_write32_set(dev, addr, bits) hw_write32((u32)(addr), hw_read32((u32)(addr)) |  (u32)(bits))
#define rtw_write32_clr(dev, addr, bits) hw_write32((u32)(addr), hw_read32((u32)(addr)) & ~(u32)(bits))

/* masked writes: data is shifted into the mask's low bit */
#define rtw_write8_mask(dev, addr, mask, data)  hw_write8 ((u32)(addr), (hw_read8 ((u32)(addr)) & ~(u8)(mask))  | ((((u8)(data))  << __builtin_ctz(mask)) & (u8)(mask)))
#define rtw_write16_mask(dev, addr, mask, data) hw_write16((u32)(addr), (hw_read16((u32)(addr)) & ~(u16)(mask)) | ((((u16)(data)) << __builtin_ctz(mask)) & (u16)(mask)))
#define rtw_write32_mask(dev, addr, mask, data) hw_write32((u32)(addr), (hw_read32((u32)(addr)) & ~(u32)(mask)) | ((((u32)(data)) << __builtin_ctz(mask)) & (u32)(mask)))

/* register #defines: pristine upstream reg.h, vendored into the tree so the
 * build is self-contained (reference/ is an untracked browse copy). */
#include "upstream/reg.h"

/* SDIO-only symbols the power-seq parser references in branches that never run
 * on PCIe (they live in upstream sdio.h, not reg.h). Defined here only so the
 * verbatim code compiles; values are irrelevant on our interface. */
#ifndef REG_SDIO_HSUS_CTRL
#define REG_SDIO_HSUS_CTRL  0x0086
#endif
#ifndef BIT_HCI_SUS_REQ
#define BIT_HCI_SUS_REQ     BIT(0)
#endif
#ifndef BIT_HCI_RESUME_RDY
#define BIT_HCI_RESUME_RDY  BIT(1)
#endif
#ifndef BIT_SDIO_PAD_E5
#define BIT_SDIO_PAD_E5     BIT(18)
#endif
#ifndef REG_SDIO_HIMR
#define REG_SDIO_HIMR       0x10250014
#endif

#endif /* RTW_SHIM_H */
