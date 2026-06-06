/*
 * rtw_hw.h — userspace hardware-access layer for the RTL8821CE, backed by the
 * RTW88Server kext (bridge/). This is the ONLY thing that talks to silicon;
 * everything above it (the lifted upstream rtw88 bring-up) is pure logic and
 * reaches the chip exclusively through these calls.
 *
 * Open the bridge once, then read/write registers, alloc DMA, wait on IRQs —
 * all from a normal userspace process. No kext rebuild, no reboot.
 */
#ifndef RTW_HW_H
#define RTW_HW_H

#include <stdint.h>
#include <stdbool.h>

/* connect to / disconnect from the bridge kext. returns 0 on success. */
int  rtw_hw_open(void);
void rtw_hw_close(void);

/* identity (PING) */
void rtw_hw_ident(uint16_t *vendor, uint16_t *device, uint32_t *bar_len);

/* BAR2 MMIO. All register access in the bring-up funnels through these. */
uint8_t  hw_read8 (uint32_t off);
uint16_t hw_read16(uint32_t off);
uint32_t hw_read32(uint32_t off);
void     hw_write8 (uint32_t off, uint8_t  v);
void     hw_write16(uint32_t off, uint16_t v);
void     hw_write32(uint32_t off, uint32_t v);

/* PCI config space */
uint32_t hw_cfg_read32(uint32_t off);

/* power / bus-master enable */
void hw_power(bool mem_enable, bool busmaster);
/* assert to the bridge that the MAC is now powered (gates IRQ-status reads) */
void hw_set_mac_power(bool on);

/* DMA: bridge owns the physical address. Returns a handle + a mapped userspace
 * pointer for the buffer contents. paddr is informational (logging). */
int  hw_dma_alloc(uint64_t size, uint64_t *handle, uint64_t *paddr, void **vaddr);
void hw_dma_free (uint64_t handle, void *vaddr, uint64_t size);
/* SAFE ring-arm: bridge writes (paddr_of(handle)+buf_off) into device reg. */
int  hw_reg_write_dma(uint32_t reg_off, uint64_t handle, uint64_t buf_off, uint32_t width);

/* interrupts (lazily armed; safe — primary path reads no device register) */
int      hw_irq_enable(void);
void     hw_irq_disable(void);
uint64_t hw_irq_wait(uint32_t timeout_ms);          /* returns #fired */
/* guarded interrupt-status read: nonzero return = refused (MAC off) */
int      hw_irq_status(uint32_t off, uint32_t width, uint32_t *val);

/* ---- in-kext data path (TX/RX run in the kext, no per-packet syscall) ----- */
struct rtw_data_cfg;
int  hw_data_start(const struct rtw_data_cfg *cfg);   /* publish enX + run data path */
void hw_data_stop(void);
void hw_data_link(int up);
/* read+reset in-kext counters: tx,txDrop,txBytes,rx,rxBytes,rxRetry,rxMcs,rxMaxDepth,
 * ba(cumulative),rxErr(crc/icv),rxParse(bad-format),seqGap,seqBack,
 * miss(per-TID missing seqs),rxRingFull,rateSum,rateN,rateMax,retryDup,tidMask */
void hw_data_stats(uint64_t out[21]);

/* trigger in-kernel power-on / full bring-up / scan / connect. */
struct rtw_scan_result;
struct rtw_connect_result;
uint32_t hw_kctl_poweron(void);   /* (ret<<16)|(CR<<8)|poweron */
uint32_t hw_kctl_bringup(void);   /* (ok<<16)|MCUFW_CTRL       */
int      hw_kctl_scan(struct rtw_scan_result *r);   /* fills r, returns count or -1 */
int      hw_kctl_connect(const char *ssid, const char *pass, struct rtw_connect_result *res);
int      hw_kctl_disconnect(void);   /* remove enX + free rings (switch back to other ifaces) */

#endif /* RTW_HW_H */
