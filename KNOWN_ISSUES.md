# Known issues / limitations

A working RTL8821CE driver, but with documented limits. The data path is hardware-only
testable (no IOMMU, real PCIe Wi-Fi), so behavioral PHY/RX/TX changes are validated one at
a time on hardware — see the notes below for where things stand.

## 1. Firmware IQK (I/Q calibration) does not run — RX is uncalibrated

**Status:** known limitation, non-fatal. The link works (~40 Mbps on 5GHz 20MHz HT).

`rtw_do_iqk()` (`core/phy.c`) sends the firmware IQK H2C exactly as upstream rtw88's
`rtw8821c_do_iqk` does and polls RF path-A reg 0x08 (`RF_DTXLOK`) for the firmware's
`0xABCDE` "done" sentinel. **The firmware never writes it** — RF 0x08 stays at its
`0x9c060` reset value, and `REG_IQKFAILMSK` (0x1bf0) reports `0x00` (not even a failure).

This was diagnosed step by step on hardware; every driver-side cause was **eliminated**:

| Hypothesis | Result |
|---|---|
| Wrong completion mechanism | ❌ — `RF_DTXLOK==0xABCDE` is upstream-exact |
| Wrong H2C packet format | ❌ — byte-for-byte identical to upstream `rtw_fw_do_iqk` |
| Poll window too short | ❌ — widened to upstream's 300×20ms (~6s); still times out |
| Hand-rolled RFK-inform wrap | ❌ — removed (upstream issues none); no change |
| Firmware too old for IQK | ❌ — firmware is **v24.11**, which supports IQK |
| RF read path can't read 0x08 | ❌ — reads a stable, sane `0x9c060` |
| H2C descriptor missing Last-Segment bit | ❌ — added (real latent bug, now fixed); no change |

**Conclusion:** the firmware *receives* a valid IQK packet but does not *execute* the
calibration. The remaining gap is a firmware bring-up **precondition** that upstream rtw88
establishes and this port does not.

**How to fix it (the only reliable path):** boot Linux on the same hardware, load `rtw88`
with debug enabled, connect to the same AP, and capture the exact H2C packets + register
writes around `rtw8821c_phy_calibration`. Diff that sequence against this port's bring-up;
the missing precondition will be apparent. The IQK code already matches upstream, so it
should "just work" once that precondition is added.

**Impact:** uncalibrated RX is tolerable at 20MHz HT but is the suspected blocker for
40MHz (see below) — a wider channel has less SNR margin, and a 40MHz association attempt
failed at AUTH while IQK was timing out.

## 2. Downlink capped at ~72 Mbps (20MHz HT, 1 spatial stream)

VHT is not advertised (`RTW_ADVERTISE_VHT 0` in `core/assoc.c`) because the 40/80MHz
baseband path isn't validated, and the 8821CE is 1×1 (no second spatial stream). So the
PHY ceiling is HT-MCS7 @20MHz +SGI = **72.2 Mbps** (~40 Mbps TCP in practice).

**40MHz (HT40)** would roughly double this (~150 Mbps PHY). The baseband scaffolding
exists (`set_channel_bb`/`_rf`/`_rxdfir` handle `WIDTH_40`), and a macro-gated attempt was
written, but it **failed at AUTH** on hardware — almost certainly because RX is
uncalibrated (issue #1). 40MHz is gated on IQK being fixed first.

## 3. Manual `-kconnect` while the GUI/daemon is connected can freeze

Running the client's `-kconnect` (e.g. `dev-reload.sh` step 6) while the auto-connect
daemon already holds the device can freeze the system — a disconnect/reconnect-conflict
when two paths drive the chip. Workaround: use `bash dev-reload.sh --no-run` (reload the
kext only) and connect via the GUI. A clean teardown fix is tracked separately.
