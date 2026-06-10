/*
 * phy.c — BB/AGC/MAC parameter tables via the phy_cond engine.
 *
 * The conditional-eval engine (rtw_phy_setup_phy_cond + check_positive +
 * rtw_parse_tbl_phy_cond) and the cfg appliers are VERBATIM from upstream
 * phy.c; the BB power-on sequence is from rtw8821c.c rtw8821c_phy_set_param.
 * The tables themselves are the vendored rtw8821c_table.c (upstream, untouched).
 *
 * Scope: BB power-on + load mac/bb/agc tables (direct MMIO), then the RF (serial)
 * table, the crystal-cap write, and calibration.
 */
#include "phy_tbl.h"

#define BIT_FEN_PCIEA  BIT(6)   /* rtw8821c.h */

/* tables (vendored rtw8821c_table.c) */
extern const struct rtw_table rtw8821c_mac_tbl;
extern const struct rtw_table rtw8821c_bb_tbl;
extern const struct rtw_table rtw8821c_agc_tbl;
extern const struct rtw_table rtw8821c_agc_btg_type2_tbl;   /* BTG-module RX AGC overlay */
extern const struct rtw_table rtw8821c_rf_a_tbl;

static u32 g_cfg_count;   /* entries applied in the current table load */

/* 8821c RF addressing (rtw8821c.c chip_info): write over SIPI, read direct MMIO */
#define INV_RF_DATA 0xffffffff
static const u32 rf_sipi_addr[2] = { 0x0c90, 0x0e90 };  /* LSSI write */
static const u32 rf_base_addr[2] = { 0x2800, 0x2c00 };  /* direct read base */

/* rtw_phy_read_rf (phy.c, verbatim): RF read is DIRECT MMIO, not serial */
u32 rtw_phy_read_rf(struct rtw_dev *rtwdev, enum rtw_rf_path rf_path, u32 addr, u32 mask)
{
    u32 direct_addr, m = mask & RFREG_MASK;
    if (rf_path >= rtwdev->hal.rf_phy_num) return INV_RF_DATA;
    addr &= 0xff;
    direct_addr = rf_base_addr[rf_path] + (addr << 2);
    return (hw_read32(direct_addr) & m) >> __builtin_ctz(m);
}

/* rtw_phy_write_rf_reg_sipi (phy.c, verbatim): RF write over the SIPI bus */
bool rtw_write_rf(struct rtw_dev *rtwdev, enum rtw_rf_path rf_path, u32 addr, u32 mask, u32 data)
{
    u32 data_and_addr, old_data = 0, shift;
    if (rf_path >= rtwdev->hal.rf_phy_num) return false;
    addr &= 0xff;
    mask &= RFREG_MASK;
    if (mask != RFREG_MASK) {                  /* read-modify-write (partial mask) */
        old_data = rtw_phy_read_rf(rtwdev, rf_path, addr, RFREG_MASK);
        if (old_data == INV_RF_DATA) return false;
        shift = __builtin_ctz(mask);
        data = (old_data & ~mask) | (data << shift);
    }
    data_and_addr = ((addr << 20) | (data & 0x000fffff)) & 0x0fffffff;
    rtw_write32(rtwdev, rf_sipi_addr[rf_path], data_and_addr);
    udelay(13);
    return true;
}

/* ---- IQK (firmware I/Q calibration) — rtw8821c_do_iqk -------------------- *
 * 8821c calibration is a single firmware H2C (no driver-side DPK/LCK/DACK): send the
 * IQK packet, then poll RF path-A reg 0x08 (RF_DTXLOK) for the firmware's 0xABCDE
 * "done" sentinel and clear it. When it runs it removes the TX/RX I/Q image that
 * otherwise corrupts a fraction of received symbols (silent FCS failures, err=0, after
 * which the AP rate-floors the downlink). Run on the connection channel once it's tuned.
 *
 * KNOWN LIMITATION (see KNOWN_ISSUES.md): on this port the firmware does NOT execute the
 * IQK. It receives a byte-perfect IQK H2C — verified on hardware against upstream rtw88:
 * identical packet format, identical H2C-ring delivery, firmware v24.11 which supports
 * IQK — yet never writes the 0xABCDE sentinel (RF 0x08 stays at its 0x9c060 reset value).
 * Every driver-side cause was eliminated step by step (completion mechanism, packet
 * format, H2C Last-Segment bit, the RFK-inform wrap, the poll window, the RF read path).
 * The remaining gap is a firmware bring-up precondition upstream establishes that this
 * port does not. RX therefore runs UNCALIBRATED — tolerable at 20MHz HT (the link works,
 * ~40 Mbps) but the suspected blocker for 40MHz. The code below matches upstream
 * rtw8821c_do_iqk, so it will work once the precondition is found (capture a live rtw88
 * H2C/register trace around phy_calibration on this hardware and diff). Non-fatal. */
extern int trx_tx_h2c(struct rtw_dev *rtwdev, const u8 *pkt);

void rtw_do_iqk(struct rtw_dev *rtwdev)
{
    /* IQK H2C packet, matching upstream rtw_fw_do_iqk: word0 = category 0x01 |
     * cmd 0xFF | sub_id IQK(0x0E); word1 = total_len(HDR 8 + 1) | seq; word2 =
     * clear(bit0)=0 + segment_iqk(bit1)=0 (full IQK — correct pre-association).
     * Upstream issues no RFK-inform mailbox around this, so neither do we. */
    static u16 seq = 2;                 /* fw_handshake used seq 0,1 at bring-up */
    u8 pkt[32] = {0};
    u32 *w = (u32 *)pkt;
    w[0] = 0x01u | (0xFFu << 8) | (0x0Eu << 16);
    w[1] = 9u | ((u32)seq++ << 16);
    w[2] = 0;
    trx_tx_h2c(rtwdev, pkt);

    int done = 0, i;
    for (i = 0; i < 300; i++) {          /* ~6s, matching upstream rtw8821c_do_iqk's 300 x 20ms */
        if (rtw_phy_read_rf(rtwdev, RF_PATH_A, 0x08, RFREG_MASK) == 0xabcde) { done = 1; break; }
        usleep_range(20000, 20000);
    }
    rtw_write_rf(rtwdev, RF_PATH_A, 0x08, RFREG_MASK, 0x0);
    /* REG_IQKFAILMSK (0x1bf0) [7:0] = per-path fail mask. On a timeout, failmask==0 means
     * the firmware never ran the calibration (the known limitation documented above). */
    rtw_info(rtwdev, "  IQK %s (failmask=0x%02x)",
             done ? "done — calibrated" : "TIMEOUT — fw did not run IQK (uncalibrated)",
             hw_read32(0x1bf0) & 0xffu);
}

/* ---- cfg appliers (phy.c, verbatim) ------------------------------------- */
void rtw_phy_cfg_mac(struct rtw_dev *rtwdev, const struct rtw_table *tbl, u32 addr, u32 data)
{ (void)tbl; rtw_write8(rtwdev, addr, data); g_cfg_count++; }

void rtw_phy_cfg_agc(struct rtw_dev *rtwdev, const struct rtw_table *tbl, u32 addr, u32 data)
{ (void)tbl; rtw_write32(rtwdev, addr, data); g_cfg_count++; }

void rtw_phy_cfg_bb(struct rtw_dev *rtwdev, const struct rtw_table *tbl, u32 addr, u32 data)
{
    (void)tbl;
    if (addr == 0xfe)      msleep(50);
    else if (addr == 0xfd) mdelay(5);
    else if (addr == 0xfc) mdelay(1);
    else if (addr == 0xfb) usleep_range(50, 60);
    else if (addr == 0xfa) udelay(5);
    else if (addr == 0xf9) udelay(1);
    else { rtw_write32(rtwdev, addr, data); g_cfg_count++; }
}

/* RF applier (phy.c, verbatim) — delay opcodes + SIPI write */
void rtw_phy_cfg_rf(struct rtw_dev *rtwdev, const struct rtw_table *tbl, u32 addr, u32 data)
{
    if (addr == 0xffe) {
        msleep(50);
    } else if (addr == 0xfe) {
        usleep_range(100, 110);
    } else {
        rtw_write_rf(rtwdev, tbl->rf_path, addr, RFREG_MASK, data);
        udelay(1);
        g_cfg_count++;
    }
}

/* bb_pg / txpwr_lmt parsers — STUBS (those tables are TX-power, not loaded for
 * RX; symbols needed by the table file's DECL initializers). */
void rtw_parse_tbl_bb_pg(struct rtw_dev *rtwdev, const struct rtw_table *tbl)
{ (void)rtwdev; (void)tbl; }
void rtw_parse_tbl_txpwr_lmt(struct rtw_dev *rtwdev, const struct rtw_table *tbl)
{ (void)rtwdev; (void)tbl; }

/* ---- conditional-eval engine (phy.c, verbatim) -------------------------- */
static void rtw_phy_setup_phy_cond(struct rtw_dev *rtwdev, u32 pkg)
{
    struct rtw_hal_min *hal = &rtwdev->hal;
    struct rtw_efuse *efuse = &rtwdev->efuse;
    struct rtw_phy_cond cond = {0};

    cond.cut = hal->cut_version ? hal->cut_version : 15;
    cond.pkg = pkg ? pkg : 15;
    cond.plat = 0x04;
    cond.rfe = efuse->rfe_option;
    cond.intf = INTF_PCIE;   /* PCIe */
    hal->phy_cond = cond;
}

static bool check_positive(struct rtw_dev *rtwdev, struct rtw_phy_cond cond)
{
    struct rtw_phy_cond drv_cond = rtwdev->hal.phy_cond;

    if (cond.cut && cond.cut != drv_cond.cut)   return false;
    if (cond.pkg && cond.pkg != drv_cond.pkg)   return false;
    if (cond.intf && cond.intf != drv_cond.intf) return false;
    /* 8821c is not 8812A/8821A: simple rfe equality */
    if (cond.rfe != drv_cond.rfe)               return false;
    return true;
}

void rtw_parse_tbl_phy_cond(struct rtw_dev *rtwdev, const struct rtw_table *tbl)
{
    const union phy_table_tile *p = tbl->data;
    const union phy_table_tile *end = p + tbl->size / 2;
    struct rtw_phy_cond pos_cond = {0};
    bool is_matched = true, is_skipped = false;

    for (; p < end; p++) {
        if (p->cond.pos) {
            switch (p->cond.branch) {
            case BRANCH_ENDIF: is_matched = true; is_skipped = false; break;
            case BRANCH_ELSE:  is_matched = is_skipped ? false : true; break;
            case BRANCH_IF:
            case BRANCH_ELIF:
            default:           pos_cond = p->cond; break;
            }
        } else if (p->cond.neg) {
            if (!is_skipped) {
                if (check_positive(rtwdev, pos_cond)) { is_matched = true; is_skipped = true; }
                else                                  { is_matched = false; is_skipped = false; }
            } else {
                is_matched = false;
            }
        } else if (is_matched) {
            (*tbl->do_cfg)(rtwdev, tbl, p->cfg.addr, p->cfg.data);
        }
    }
}

/* ---- rtw8821c_phy_set_param (BB power-on + table load; rf/crystal) ------- */
static u32 load_one(struct rtw_dev *rtwdev, const struct rtw_table *tbl, const char *name)
{
    g_cfg_count = 0;
    rtw_load_table(rtwdev, tbl);
    rtw_info(rtwdev, "  loaded %-4s table: %u entries applied", name, g_cfg_count);
    return g_cfg_count;
}

int phy_set_param(struct rtw_dev *rtwdev)
{
    u8 val;

    rtw_phy_setup_phy_cond(rtwdev, rtwdev->hal.pkg_type);

    /* power on BB/RF domain (rtw8821c_phy_set_param) */
    val = rtw_read8(rtwdev, REG_SYS_FUNC_EN);
    val |= BIT_FEN_PCIEA;
    rtw_write8(rtwdev, REG_SYS_FUNC_EN, val);

    /* toggle BB reset */
    val |= BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST;
    rtw_write8(rtwdev, REG_SYS_FUNC_EN, val);
    val &= ~(BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST);
    rtw_write8(rtwdev, REG_SYS_FUNC_EN, val);
    val |= BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST;
    rtw_write8(rtwdev, REG_SYS_FUNC_EN, val);

    rtw_write8(rtwdev, REG_RF_CTRL, BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB);
    usleep_range(10, 11);
    rtw_write8(rtwdev, REG_WLRF1 + 3, BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB);
    usleep_range(10, 11);

    /* pre init before header files config */
    rtw_write32_clr(rtwdev, REG_RXPSEL, BIT_RX_PSEL_RST);

    /* load tables (mac/bb/agc direct MMIO, then rf via SIPI) */
    load_one(rtwdev, &rtw8821c_mac_tbl, "mac");
    u32 bb  = load_one(rtwdev, &rtw8821c_bb_tbl, "bb");
    u32 agc = load_one(rtwdev, &rtw8821c_agc_tbl, "agc");

    /* BTG-module RX AGC overlay (rtw_phy_load_tables: rfe_def->agc_btg_tbl).
     * Upstream's rtw8821c_rfe_defs maps ONLY rfe_option 2 and 4 to
     * rtw8821c_agc_btg_type2_tbl; those modules route 2.4GHz through the BT-shared
     * front-end whose LNA needs different RX gain tables. Without this overlay the
     * 2.4GHz RX runs the default (WLG) gain curve -> poor sensitivity, downlink
     * collapses to near-zero (the ~40kbps / ping-timeout symptom). 5GHz is unaffected. */
    if (rtwdev->efuse.rfe_option == 0x2 || rtwdev->efuse.rfe_option == 0x4)
        agc += load_one(rtwdev, &rtw8821c_agc_btg_type2_tbl, "agcB");

    /* RF: read RF reg 0x00 before/after the table load as an
     * RF-alive check (RF read is direct MMIO at rf_base + addr*4). */
    u32 rf0_before = rtw_phy_read_rf(rtwdev, RF_PATH_A, 0x00, RFREG_MASK);
    u32 rf = load_one(rtwdev, &rtw8821c_rf_a_tbl, "rf");
    u32 rf0_after = rtw_phy_read_rf(rtwdev, RF_PATH_A, 0x00, RFREG_MASK);
    rtw_info(rtwdev, "  RF[0x00] before=0x%05x after=0x%05x", rf0_before, rf0_after);

    /* crystal cap + AFE (rtw8821c_phy_set_param post-table) */
    u8 crystal_cap = rtwdev->efuse.crystal_cap & 0x3F;
    rtw_write32_mask(rtwdev, REG_AFE_XTAL_CTRL, 0x7e000000, crystal_cap);
    rtw_write32_mask(rtwdev, REG_AFE_PLL_CTRL, 0x7e, crystal_cap);
    rtw_write32_mask(rtwdev, REG_CCK0_FAREPORT, BIT(18) | BIT(22), 0);

    /* post init after header files config */
    rtw_write32_set(rtwdev, REG_RXPSEL, BIT_RX_PSEL_RST);

    /* correctness: cond engine matched + RF responds (reg 0x00 not 0/0xfffff) */
    int rf_alive = (rf0_after != 0 && rf0_after != 0xfffff);
    return (bb > 100 && agc > 100 && rf > 100 && rf_alive) ? 0 : -1;
}
