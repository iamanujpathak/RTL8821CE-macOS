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
#define COEX_GNT_SW_LOW       0x1
#define COEX_GNT_SW_HIGH      0x3

static u32 lte_read(struct rtw_dev *d, u16 off)
{
    if (!check_hw_ready(d, LTECOEX_CTRL, LTECOEX_READY, 1)) return 0;
    hw_write32(LTECOEX_CTRL, 0x800F0000 | off);     /* read access cmd */
    return hw_read32(LTECOEX_RDATA);
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
    u32 tmp = lte_read(d, addr);
    tmp = (tmp & ~mask) | ((val << shift) & mask);
    lte_write(d, addr, tmp);
}

void coex_wifi_antenna(struct rtw_dev *rtwdev)
{
    /* coex_ctrl_owner(wifi=true): driver/SW controls the antenna mux, not PTA */
    rtw_write8_set(rtwdev, REG_SYS_SDIO_CTRL + 3, BIT_LTE_MUX_CTRL_PATH >> 24);

    /* GNT_WL = SW HIGH (force WiFi the antenna) */
    indirect_write(rtwdev, LTE_COEX_CTRL, 0x3000, COEX_GNT_SW_HIGH);
    indirect_write(rtwdev, LTE_COEX_CTRL, 0x0300, COEX_GNT_SW_HIGH);
    /* GNT_BT = SW LOW */
    indirect_write(rtwdev, LTE_COEX_CTRL, 0xc000, COEX_GNT_SW_LOW);
    indirect_write(rtwdev, LTE_COEX_CTRL, 0x0c00, COEX_GNT_SW_LOW);
}
