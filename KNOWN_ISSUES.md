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

Current working-tree instrumentation (`core/phy.c`, `core/coex.c`) adds the coex
scoreboard/PTA bring-up upstream always performs before IQK, plus H2C-FIFO read/write
pointer diagnostics, so the next hardware boot can distinguish "MCU never parsed the
packet" from "parsed but refused to calibrate".

**How to fix it (the only reliable path):** boot Linux on the same hardware, load `rtw88`
with debug enabled, connect to the same AP, and capture the exact H2C packets + register
writes around `rtw8821c_phy_calibration`. Diff that sequence against this port's bring-up;
the missing precondition will be apparent. The IQK code already matches upstream, so it
should "just work" once that precondition is added.

**Impact:** uncalibrated RX is tolerable at 20MHz HT but is the suspected blocker for
40MHz (see below) — a wider channel has less SNR margin, and a 40MHz association attempt
failed at AUTH while IQK was timing out.

## 2. Downlink capped at ~72 Mbps PHY (20MHz HT, 1 spatial stream)

VHT is not advertised (`RTW_ADVERTISE_VHT 0` in `core/assoc.c`) because the 40/80MHz
baseband path isn't validated, and the 8821CE is 1×1 (no second spatial stream). So the
PHY ceiling is HT-MCS7 @20MHz +SGI = **72.2 Mbps** (~40 Mbps TCP in practice).

**40MHz (HT40)** would roughly double this (~150 Mbps PHY). The baseband scaffolding
exists (`set_channel_bb`/`_rf`/`_rxdfir` handle `WIDTH_40`), and a macro-gated attempt was
written, but it **failed at AUTH** on hardware — almost certainly because RX is
uncalibrated (issue #1). 40MHz is gated on IQK being fixed first. (80MHz is not ported at
all; `set_channel` now refuses it explicitly and falls back to 20MHz.)

## 3. ~~Manual `--kconnect` while the daemon is connected can freeze~~ (mitigated)

Two control paths driving the chip concurrently could freeze the system. Now mitigated at
three levels:

1. The CLI and the GUI both route through the `rtwd` daemon (one process owns the kext),
   and the daemon enforces a single instance via an `flock`'d lock file.
2. The `--k*` diagnostics **refuse to run while the daemon is up** (override: `--force`).
3. The kext serializes the **high-level control-plane methods** (`kBridgeK*` connect/
   scan/disconnect/status + the `dataStart/Stop/Link` family) behind a lock, so two
   clients can no longer interleave a connect/scan/disconnect or double-free the data
   path. NOTE: the *raw* primitives (`kBridgeRegWrite`, `kBridgeSetMacPower`, etc.) are
   **not** under that lock — they are only used by the in-kernel bring-up, but a
   misbehaving client could still drive them concurrently. They are admin-gated.

The serialization (3) is new and pending hardware validation, but it can only make the
concurrent case safer — the single-client path takes the same lock uncontended.

## 3b. Open (unencrypted) networks are not supported — WPA2-PSK only

The in-kernel data path encrypts every TX frame with hardware CCMP (Protected bit +
sec_type), so it requires a pairwise key from the 4-way handshake. An open network has no
key: association succeeds but the data path never starts, so the join reports failure.
The GUI shows open networks but labels them unsupported rather than offering a Connect
button that always fails. Supporting them needs an unencrypted in-kext TX path
(`txEthFrame` without the CCMP header) — tracked, not yet done.

## 4. Pending hardware validation (recent fixes)

These landed from a code review and are correct against upstream rtw88 sources, but the
data path is hardware-only testable, so treat them as "validate on next boot":

- **EN_HWSEQ on the right bit (BIT 15)** for the in-kext TX descriptors
  (`server/RTW88Server.cpp`) — previously every in-kext data/ADDBA frame transmitted
  with sequence number 0. Expected to *improve* Block-Ack/de-dup behavior noticeably.
- **GTK extraction failure now fails the handshake** (`core/wpa.c`) instead of silently
  connecting without broadcast RX.
- **Deauth (reason 3) is sent to the AP on disconnect** (`server/kctl.c`).
- **Every scan re-enables PCI bus-mastering on entry** (`server/kctl.c`) — previously the
  second and later `kBridgeKScan` calls ran with DMA off and found 0 networks, which
  permanently wedged rtwd's auto-connect.
- **ADDBA-response/M4-retransmit handling** (`core/wpa.c`).
- **Receive-identity registers programmed before the exchange** (`core/assoc.c` writes
  REG_MACID/REG_BSSID/REG_RRSR before AUTH, so the WMAC auto-ACKs our unicast during the
  whole AUTH/ASSOC/4-way, not just after) — a hardware-behavioral change.
- **CCK TX-filter parameters cached once from the BB-table defaults** (`core/channel.c`)
  instead of re-read on every `set_channel` (a channel-14 tune previously poisoned the
  filter for channels 1-13 for the rest of the session). The cache only latches once it
  reads plausible (non-zero, non-0xffffffff) table values, so a pre-table or failed
  bring-up can't poison it.

## 5. Improvements tracked, not yet implemented

Known gaps that need dedicated hardware time (in rough priority order):

- **TX power is a fixed test value** — `set_channel` writes a flat mid-power TX AGC for
  12 legacy rates only; HT/VHT MCS power and efuse/regulatory-driven power tables are not
  programmed (upstream `rtw8821c_set_tx_power_index_by_rate`).
- **`rtw_set_channel_mac` is skipped** — upstream also programs REG_DATA_SC + RFMOD
  bandwidth bits at channel set.
- **BTG boards (rfe_option 7/0xa/0xc/0xf) don't get the BTG AGC overlay** on 2.4GHz.
- **No RSSI in scan results** — the GUI's signal bars are a beacon-count proxy; the RX
  descriptor's phy-status block (where RSSI lives) is not parsed yet.
- **Control-path TX shares one bounce buffer** (`core/trx.c g_pkt_vaddr`) with no
  TX-completion check; fine for the strictly sequential control flow, but a second
  in-flight control frame would overwrite the first.
- **H2C mailbox box counter survives chip re-power** (`core/media.c`) — harmless in
  practice (the TFR-empty wait covers it) but diverges from upstream's reset.
