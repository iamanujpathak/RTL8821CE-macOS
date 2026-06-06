/*
 * phy_tbl.h — just enough of upstream main.h/phy.h for the vendored
 * rtw8821c_table.c to compile verbatim: struct rtw_table, the table-pair
 * structs, ARRAY_SIZE, and the RTW_DECL_TABLE_* macros. The table arrays stay
 * untouched; only the engine (phy.c) interprets them.
 */
#ifndef PHY_TBL_H
#define PHY_TBL_H

#include "rtw_shim.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

struct rtw_table {
    const void *data;
    const u32 size;
    void (*parse)(struct rtw_dev *rtwdev, const struct rtw_table *tbl);
    void (*do_cfg)(struct rtw_dev *rtwdev, const struct rtw_table *tbl, u32 addr, u32 data);
    enum rtw_rf_path rf_path;
};

/* power-by-gain + txpwr-limit pairs (declared by the table file; not used for RX) */
struct rtw_phy_pg_cfg_pair { u32 band, rf_path, tx_num, addr, bitmask, data; };
struct rtw_txpwr_lmt_cfg_pair { u8 regd, band, bw, rs, ch; s8 txpwr_lmt; };

/* appliers + parsers (defined in phy.c) */
void rtw_phy_cfg_mac(struct rtw_dev *, const struct rtw_table *, u32 addr, u32 data);
void rtw_phy_cfg_agc(struct rtw_dev *, const struct rtw_table *, u32 addr, u32 data);
void rtw_phy_cfg_bb(struct rtw_dev *, const struct rtw_table *, u32 addr, u32 data);
void rtw_phy_cfg_rf(struct rtw_dev *, const struct rtw_table *, u32 addr, u32 data);
void rtw_parse_tbl_phy_cond(struct rtw_dev *, const struct rtw_table *);
void rtw_parse_tbl_bb_pg(struct rtw_dev *, const struct rtw_table *);
void rtw_parse_tbl_txpwr_lmt(struct rtw_dev *, const struct rtw_table *);

static inline void rtw_load_table(struct rtw_dev *rtwdev, const struct rtw_table *tbl)
{ (*tbl->parse)(rtwdev, tbl); }

/* table-declaration macros (phy.h, verbatim) */
#define RTW_DECL_TABLE_PHY_COND_CORE(name, cfg, path) \
const struct rtw_table name ## _tbl = { \
    .data = name, .size = ARRAY_SIZE(name), \
    .parse = rtw_parse_tbl_phy_cond, .do_cfg = cfg, .rf_path = path, }
#define RTW_DECL_TABLE_PHY_COND(name, cfg) RTW_DECL_TABLE_PHY_COND_CORE(name, cfg, 0)
#define RTW_DECL_TABLE_RF_RADIO(name, path) \
    RTW_DECL_TABLE_PHY_COND_CORE(name, rtw_phy_cfg_rf, RF_PATH_ ## path)
#define RTW_DECL_TABLE_BB_PG(name) \
const struct rtw_table name ## _tbl = { \
    .data = name, .size = ARRAY_SIZE(name), .parse = rtw_parse_tbl_bb_pg, }
#define RTW_DECL_TABLE_TXPWR_LMT(name) \
const struct rtw_table name ## _tbl = { \
    .data = name, .size = ARRAY_SIZE(name), .parse = rtw_parse_tbl_txpwr_lmt, }

#endif /* PHY_TBL_H */
