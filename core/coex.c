/*
 * coex.c — give WiFi the shared antenna so TX can radiate.
 *
 * The 8821CE is a combo WiFi+BT chip with a shared antenna; efuse btcoex=true.
 * WiFi TX only goes out when GNT_WL (grant-WiFi) is asserted. Normally the
 * BT-coex PTA arbitrates that; with no coex init the antenna isn't granted to
 * WiFi, so RX works but TX is silent. This forces WiFi-owns-antenna:
 *   coex_ctrl_owner(wifi) + GNT_WL=SW_HIGH + GNT_BT=SW_LOW.
 *
 * GNT writes go through the ltecoex indirect interface (rtw88 coex.c verbatim
 * protocol): poll READY, write data, issue the access command.
 */
#include "trx_regs.h"

#define LTECOEX_CTRL          0x1700   /* REG_WL2LTECOEX_..._CTRL_V1   */
#define LTECOEX_WDATA         0x1704
#define LTECOEX_RDATA         0x1708
#define LTECOEX_READY         BIT(29)
#define LTE_COEX_CTRL         0x38     /* indirect reg holding GNT bits */
#define REG_SYS_SDIO_CTRL     0x0070
#define BIT_LTE_MUX_CTRL_PATH BIT(26)
#define COEX_GNT_HW_PTA       0x0    /* GNT under PTA/hardware control (coex.h) */
#define COEX_GNT_SW_LOW       0x1
#define COEX_GNT_SW_HIGH      0x3
/* coex scoreboard bits (coex.h). REG_WIFI_BT_INFO + BIT_BT_INT_EN come from reg.h. */
#define COEX_SCBD_ACTIVE      0x0001
#define COEX_SCBD_ONOFF       0x0002
#define BT_CNT_ENABLE         0x1      /* rtw8821c.h:264 — REG_BT_STAT_CTRL value */

static int lte_read(struct rtw_dev *d, u16 off, u32 *out)
{
    if (!check_hw_ready(d, LTECOEX_CTRL, LTECOEX_READY, 1)) return -1;
    hw_write32(LTECOEX_CTRL, 0x800F0000 | off);     /* read access cmd */
    *out = hw_read32(LTECOEX_RDATA);
    return 0;
}
static void lte_write(struct rtw_dev *d, u16 off, u32 val)
{
    if (!check_hw_ready(d, LTECOEX_CTRL, LTECOEX_READY, 1)) return;
    hw_write32(LTECOEX_WDATA, val);
    hw_write32(LTECOEX_CTRL, 0xC00F0000 | off);     /* write access cmd */
}
static void indirect_write(struct rtw_dev *d, u16 addr, u32 mask, u32 val)
{
    u32 shift = __builtin_ctz(mask);
    u32 tmp;
    /* a failed read must ABORT, not proceed with tmp=0 — that would zero every other
     * GNT field in the indirect register instead of updating just `mask` */
    if (lte_read(d, addr, &tmp) != 0) {
        rtw_warn(d, "ltecoex not ready — skipping GNT write (reg 0x%x mask 0x%x)", addr, mask);
        return;
    }
    tmp = (tmp & ~mask) | ((val << shift) & mask);
    lte_write(d, addr, tmp);
}

void coex_wifi_antenna(struct rtw_dev *rtwdev)
{
    /* Minimal firmware-visible coex bring-up before the first IQK. On this BT-combo chip
     * the WLAN firmware's RF calibration arbitrates against the BT core: upstream's
     * rtw_coex_check_rfk polls REG_ARFR4 BIT_WL_RFK against the BT-RFK scoreboard bit, and
     * upstream ALWAYS runs rtw_coex_init_hw_config (scoreboard ACTIVE|ONOFF, then ant-switch
     * with GNT_BT under the PTA at 5G) before any IQK. The port did none of this and forced
     * GNT_BT SW-LOW, leaving the fw's BT interlock uninitialised — the leading suspect for
     * "fw consumes the IQK H2C but never calibrates (failmask 0)". Restore the minimal state:
     *   (0) ENABLE the coex hardware block (PTA + BT-stat + GNT-out-of-debug) so the
     *       scoreboard register latches and GNT_BT=HW_PTA is meaningful (the confirmed gap:
     *       last boot's scbd readback was 0x0000 because the block was inert);
     *   (1) tell the fw WL is active via the scoreboard (rtw_coex_write_scbd ACTIVE|ONOFF);
     *   (2) keep GNT_WL SW-forced HIGH (WiFi owns the antenna) but hand GNT_BT to the PTA
     *       (COEX_SET_ANT_5G) so the WL-RFK interlock can resolve instead of waiting forever
     *       on a BT core that can never grant. The IQK diagnostics in phy.c (wl_rfk/scbd)
     *       confirm on the next boot whether this lets the firmware start calibration. */

    /* (0) ENABLE the coex hardware block first — without this the scoreboard register
     * doesn't latch (reads 0) and GNT_BT=HW_PTA is meaningless, because the BT-coex/PTA
     * block is inert. These are the essential writes from upstream rtw8821c_coex_cfg_init
     * (run inside rtw_coex_init_hw_config) + the GNT-out-of-debug clear from
     * rtw8821c_coex_cfg_gnt_debug (run inside rtw_coex_power_on_setting). Verbatim register
     * addresses/bits from upstream reg.h; the port skipped all of this entirely. */
    rtw_write32_set(rtwdev, REG_GPIO_MUXCFG, BIT_BT_PTA_EN | BIT_PO_BT_PTA_PINS); /* PTA 3-wire (BT side)  */
    rtw_write8_set(rtwdev, REG_QUEUE_CTRL, BIT_PTA_WL_TX_EN);                      /* PTA tx/rx (WiFi side) */
    rtw_write8_clr(rtwdev, REG_QUEUE_CTRL, BIT_PTA_EDCCA_EN);                      /* wl tx not gated by EDCCA */
    rtw_write8(rtwdev, REG_BT_STAT_CTRL, BT_CNT_ENABLE);                           /* enable BT counters    */
    rtw_write8_mask(rtwdev, REG_BT_TDMA_TIME, BIT_MASK_SAMPLE_RATE, 0x5);          /* BT report sample rate */
    rtw_write16_set(rtwdev, REG_BT_COEX_V2, BIT_GNT_BT_POLARITY);                  /* GNT_BT polarity       */
    rtw_write8_set(rtwdev, REG_BCN_CTRL, BIT_EN_BCN_FUNCTION);                     /* enable TBTT interrupt */
    rtw_write32_clr(rtwdev, REG_SYS_SDIO_CTRL, BIT_DBG_GNT_WL_BT);                 /* GNT out of debug mode */

    /* (1) scoreboard: upstream rtw_coex_write_scbd always sets base bit 0x2, ORs the
     * requested bits (ACTIVE|ONOFF), and gates the BT interrupt with BIT_BT_INT_EN -> 0x8003 */
    rtw_write16(rtwdev, REG_WIFI_BT_INFO,
                (u16)(0x2 | COEX_SCBD_ACTIVE | COEX_SCBD_ONOFF | BIT_BT_INT_EN));
    /* DIAGNOSTIC: read the scoreboard straight back. If this reads ~0x8003 the write
     * latches (and a later scbd=0 at IQK time means scan/another step cleared it); if it
     * reads 0x0000 the coex hardware block isn't powered (port skips rtw_coex_power_on_setting). */
    rtw_info(rtwdev, "  coex: scbd write 0x8003 -> readback 0x%04x", rtw_read16(rtwdev, REG_WIFI_BT_INFO));

    /* coex_ctrl_owner(wifi=true): driver/SW controls the antenna mux, not PTA */
    rtw_write8_set(rtwdev, REG_SYS_SDIO_CTRL + 3, BIT_LTE_MUX_CTRL_PATH >> 24);

    /* (2) GNT_WL = SW HIGH (force WiFi the antenna) */
    indirect_write(rtwdev, LTE_COEX_CTRL, 0x3000, COEX_GNT_SW_HIGH);
    indirect_write(rtwdev, LTE_COEX_CTRL, 0x0300, COEX_GNT_SW_HIGH);
    /* GNT_BT = HW/PTA (was SW_LOW), mirroring upstream COEX_SET_ANT_5G */
    indirect_write(rtwdev, LTE_COEX_CTRL, 0xc000, COEX_GNT_HW_PTA);
    indirect_write(rtwdev, LTE_COEX_CTRL, 0x0c00, COEX_GNT_HW_PTA);
}
