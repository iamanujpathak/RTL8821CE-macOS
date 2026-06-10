# RTW88 for macOS — RTL8821CE Wi-Fi as system networking

Working macOS networking for the **Realtek RTL8821CE** (`pci10ec,c821`) PCIe
Wi-Fi card on Intel Macs / Hackintoshes. The **whole Wi-Fi driver runs in a kext**
— power-on, firmware, PHY/RF, scan, WPA2 4-way handshake, hardware CCMP, and the
TX/RX data path are all in-kernel — and it presents the link to macOS as a normal
Ethernet interface (`enX`), so the **entire system** (Safari, CLI, everything)
routes through the card. A single userspace daemon (`rtwd`) tells the kext what
to do; a HeliPort-style menu-bar app and a CLI drive the daemon.

## What works

Verified end-to-end on real hardware:

- **Full connectivity** — scan → WPA2-PSK association → the kext publishes a real
  macOS Ethernet interface (`enX`) and runs the entire TX/RX data path in-kernel;
  macOS runs its own DHCP/IP on `enX` and routes the whole system through it.
- **~40 Mbps TCP** on 5 GHz (20 MHz HT). The PHY ceiling is HT-MCS7 @20 MHz
  +SGI = 72.2 Mbps — see [KNOWN_ISSUES.md](KNOWN_ISSUES.md) for why 40/80 MHz
  (and therefore faster links) are not enabled yet.
- **Menu-bar app** — scan, join (with saved passwords), forget, radio on/off,
  live connection status; anchored correctly even in full-screen spaces.
- **Auto-connect on boot** — `rtwd` joins the strongest saved network at boot and
  rejoins after drops (saved networks live in `/usr/local/etc/rtw88.conf`, 0600).
- **Multiple known networks** — it joins the strongest in range (roams across
  home/office/hotspot on restart).

## What doesn't work yet / limitations

See **[KNOWN_ISSUES.md](KNOWN_ISSUES.md)** for the full list with diagnosis
notes. Headlines:

- **Firmware IQK (RX calibration) does not run** — the link works, but RX is
  uncalibrated; this is the suspected blocker for 40 MHz channels.
- **Downlink capped at ~72 Mbps PHY** (20 MHz HT, 1 spatial stream) until the
  40 MHz baseband path is validated.
- **No GTK-rekey handling yet** — an AP's periodic group-key update can drop the
  link after ~1 hour; auto-reconnect then rejoins.

## How it works

| Component | Role |
|-----------|------|
| **`RTW88Server.kext`** (`server/` + `core/`) | **the driver.** Matches the card, maps its MMIO (BAR2), owns DMA, and runs the full 802.11 stack in-kernel: power-on, firmware, PHY/RF, scan, auth/assoc, WPA2 4-way + hardware CCMP, and the TX/RX data path. Presents the link as a macOS `IOEthernetController` (`enX`) and exposes a small, admin-only control ABI (scan / connect / disconnect / status). |
| **`rtwd`** (`client/`) | **the control daemon + CLI**, one binary. With no arguments it runs as the LaunchDaemon: the *only* process that talks to the kext — it auto-connects to saved networks, keeps the link up, and serves a tiny line/JSON protocol on `/var/run/rtw88d.sock`. With a subcommand (`rtwd scan`, `rtwd connect …`) it is a CLI client of that daemon (falling back to direct kext access when no daemon runs). |
| **`RTW88Menu.app`** (`client/gui/`) | **the menu-bar UI.** Pure UI, runs as the console user, speaks the same socket protocol as the CLI. |

Funneling every control path through one daemon is what prevents two processes
from driving the chip concurrently (historically a system-freeze).

**No IOMMU by design.** The target machines run `npci=0x2000` +
`DisableIoMapper=true`, so the device bus-masters to raw physical addresses.
The kext therefore owns all DMA addressing: buffers are allocated below 4 GB,
physically contiguous, wired for the kext's lifetime, and the only way a
physical address reaches a device register is a kext-validated arm call (raw
writes to the DMA ring-base registers are refused, and the user client itself
requires admin privilege).

## Quick start

Prebuilt releases for each push to `main` are on the
[Releases](../../releases) page — download `RTW88-macOS.zip`, unzip, then
`sudo bash install.sh --gui`. To build from source instead:

```sh
sudo bash dist/install.sh --gui     # kext + rtwd daemon + menu-bar app
#   or without --gui for a headless install (daemon + CLI only)
```

First-ever kext install: approve "RTW88Server" in **System Settings → Privacy &
Security**, reboot once. Then click the Wi-Fi icon in the menu bar to join a
network — or from a terminal:

```sh
rtwd connect "MyWiFi" "my-password"   # joins + saves it for auto-connect
rtwd status
```

For OpenCore kext injection and the no-IOMMU prerequisites:
**[dist/OpenCore.md](dist/OpenCore.md)**. To remove everything:
`sudo bash dist/uninstall.sh`.

## CLI

`rtwd <command>` drives the running daemon (and falls back to the kext when no
daemon runs):

- `rtwd scan` — list visible networks (SSID, band, channel, security, saved).
- `rtwd connect SSID [PASS] [--nosave]` — join; the saved password is reused if
  omitted; `--nosave` skips remembering it.
- `rtwd disconnect` / `rtwd status` / `rtwd forget SSID` / `rtwd power on|off`.
- `rtwd --kpoweron | --kbringup | --kscan | --kconnect | --kstats` — low-level
  kext diagnostics for development. These are **refused while the daemon runs**
  (stop it first, or pass `--force`) so they can't race the daemon on the chip.

## Configuration

`rtwd` saves every network you join to `/usr/local/etc/rtw88.conf` (mode 0600 —
it holds passphrases) and rejoins the strongest one automatically. You can also
pre-list networks by hand (see
[client/rtw88.conf.example](client/rtw88.conf.example)):

```ini
network  = MyHomeWiFi
password = home-pass
network  = Phone Hotspot
password = hotspot-pass
band     = both
```

## Repository layout

```
core/      the driver logic — portable rtw88-derived bring-up + 802.11 (power, fw,
           PHY/RF, scan, assoc, WPA2, TRX rings) + vendored rtw88 tables + firmware
           blob. Compiled into the kext; also holds the shared interface headers
           (core/rtw_hw.h, core/config.h).
server/    the macOS kext: RTW88Server.cpp (PCI/DMA/enX/user-client + in-kext data
           path), kctl.c (in-kernel control entry points), rtw88_abi.h (the
           user-client ABI shared with client/), Info.plist.
client/    rtwd — the unified daemon + CLI (main.c entry, rtwd.c daemon, config.c
           persistence, rtw_hw.c kext ABI) and gui/ (RTW88Menu.swift menu-bar app).
crypto/    bundled SHA1/HMAC/PBKDF2/AES (no CommonCrypto; used in-kernel).
dist/      install/uninstall scripts, OpenCore guide, launchd plists, netcfg helper.
build/     compiler output (git-ignored).
```

## Development

`dev-reload.sh` rebuilds both targets, reinstalls + reloads the kext, and runs a
diagnostic — the fast inner loop while iterating on the driver. Run it as your
**normal user** (it sudo's the privileged steps itself; running the whole script
under sudo leaves root-owned files in `build/`):

```sh
bash dev-reload.sh                    # rebuild + reload kext, run --kpoweron
bash dev-reload.sh --kscan            # …then run a scan instead
bash dev-reload.sh --no-run           # reload only; connect via the GUI/daemon
CLIENT_ONLY=1 bash dev-reload.sh      # skip the kext rebuild/reload
```

`server/load.sh` is the one-shot build → sign → load for the kext alone.

Hardware-validation note: PHY/RF/data-path behavior can only be validated on
real hardware (there is no emulator for this card), so behavioral changes are
landed one at a time — see the notes in [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## Future plans

- **Durability** — handle EAPOL GTK rekey in-kernel (re-derive the group key),
  add a beacon-miss link watchdog, then roaming.
- **Throughput** — fix the firmware IQK precondition (KNOWN_ISSUES #1), then
  40 MHz channel bonding; RSSI plumbed into scan results.
- **Native UI (optional)** — an IO80211 front-end over the in-kernel MAC for
  native Wi-Fi UI, quarantined as a front-end so the private API never touches
  the core driver.

## Credits & license

Radio bring-up is lifted from the Linux **rtw88** driver (Realtek/kernel
authors). See [LICENSE](LICENSE). This is an independent hobbyist project, not
affiliated with Realtek or Apple. Hardware needed: an RTL8821CE on an Intel
Mac / Hackintosh booted without an IOMMU (`npci=0x2000`, `DisableIoMapper`).
