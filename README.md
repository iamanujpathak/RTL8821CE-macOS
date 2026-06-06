# RTW88 for macOS — RTL8821CE Wi-Fi as system networking

Working macOS networking for the **Realtek RTL8821CE** (`pci10ec,c821`) PCIe
Wi-Fi card on Intel Macs / Hackintoshes. The **whole Wi-Fi driver runs in a kext**
— power-on, firmware, PHY/RF, scan, WPA2 4-way handshake, hardware CCMP, and the
TX/RX data path are all in-kernel — and it presents the link to macOS as a normal
Ethernet interface (`enX`), so the **entire system** (Safari, CLI, everything)
routes through the card. A small userspace utility just tells the kext what to do.

## What works

Verified end-to-end on real hardware:

- **Full connectivity** — scan → WPA2-PSK association → the kext publishes a real
  macOS Ethernet interface (`enX`) and runs the entire TX/RX data path in-kernel;
  macOS runs its own DHCP/IP on `enX` and routes the whole system through it.
- **Everything in-kernel** — the kext does power-on, firmware download, PHY/RF setup,
  scan, auth/assoc, the WPA2 4-way handshake and hardware-CCMP key install, and the
  data path. No per-packet syscalls.
- **Multiple known networks** — list several SSIDs; it joins the strongest in range
  (roams across home/office/hotspot on restart).
- **Auto-connect on boot** — via a LaunchDaemon + OpenCore kext injection
  (see [dist/OpenCore.md](dist/OpenCore.md)).

## What doesn't work yet / limitations

- **No GTK-rekey survival on long sessions.** An AP's periodic group-key update is
  not yet handled, so the link can silently die after ~1 hour. No auto-reconnect or
  roaming mid-session yet.
- **Throughput ~2.5 Mbps.** The hardware/link are not the ceiling (the same machine
  does ~88 Mbps on Windows); the missing per-TID RX reorder buffer and rate-control
  feedback are.

## How it works

| Component | Role |
|-----------|------|
| **`RTW88Server.kext`** (`server/` + `core/`) | **the driver.** Matches the card, maps its MMIO (BAR2), owns DMA, and runs the full 802.11 stack in-kernel: power-on, firmware, PHY/RF, scan, auth/assoc, WPA2 4-way + hardware CCMP, and the TX/RX data path. Presents the link as a macOS `IOEthernetController` (`enX`) and exposes a small control ABI (scan / connect / disconnect / status). |
| **`RTW88Client`** (`client/`) | **a thin control utility.** No radio logic — it issues scan/connect/disconnect/status commands over the kext's user-client ABI and brings the resulting `enX` online (finds it by MAC, hands it to `configd` for DHCP). |

The driver source lives in `core/` (portable rtw88-derived bring-up + 802.11) and is
compiled into the kext by `server/`; the client links only the two shared interface
headers (`core/rtw_hw.h`, `core/config.h`).

**No IOMMU by design.** The machine runs `npci=0x2000` + `DisableIoMapper=true`,
so the kext owns all DMA addressing and programs the device with validated
physical addresses — there is no userspace bus-master path at all.

## Quick start

Prebuilt `RTW88Server.kext` + `RTW88Client` for each push to `main` are on the
[Releases](../../releases) page. To build from source instead:

```sh
# 1. build
bash server/build.sh        # -> dist/RTW88Server.kext   (the driver)
bash client/build.sh        # -> build/client/RTW88Client (control utility)

# 2. load the kext (first time needs approval + reboot; OpenCore can inject it
#    instead — see dist/OpenCore.md). Then connect:
sudo build/client/RTW88Client --config rtw88.conf
#   or a one-off:  sudo build/client/RTW88Client --kconnect MyWiFi secret
```

For unattended auto-connect on boot, plus the OpenCore kext-injection and
no-IOMMU prerequisites: **[dist/OpenCore.md](dist/OpenCore.md)**, or just
`sudo bash dist/install.sh`.

## Control utility commands

`RTW88Client` reads a config file / CLI flags and drives the kext:

- `--config <file>` / `--ssid` + `--password` — scan, join the strongest configured
  network, bring `enX` online, then stay resident (the default, daemon-friendly flow).
- `--mode scan` — list visible networks and exit (no association).
- `--kscan` / `--kconnect SSID PASS` / `--kdisconnect` — drive a single kext
  operation directly.
- `--kstats` — poll the in-kernel data-path counters while traffic flows.
- `--kpoweron` / `--kbringup` — bring-up diagnostics.

Run `RTW88Client --help` for all flags.

## Configuration

List one or more networks (it joins the strongest in range — roams across
home/office/hotspot) in `/usr/local/etc/rtw88.conf`
(see [client/rtw88.conf.example](client/rtw88.conf.example)):

```ini
network  = MyHomeWiFi
password = home-pass
network  = Phone Hotspot
password = hotspot-pass
band     = both
mode     = eth
```

## Repository layout

```
core/      the driver — portable rtw88-derived bring-up + 802.11 (power, fw, PHY/RF,
           scan, assoc, WPA2, data path) + vendored rtw88 tables + firmware blob.
           Compiled into the kext; also holds the shared ABI/config headers.
server/    the macOS kext wrapper around core/: RTW88Server.cpp (PCI/DMA/enX/user-
           client) + kctl.c (in-kernel control entry points) + Info.plist + rtw88_abi.h
client/    RTW88Client — the thin control utility (main, config, user-client ABI)
crypto/    bundled SHA1/HMAC/PBKDF2/AES (no CommonCrypto; used in-kernel)
dist/      installer, OpenCore guide, auto-connect LaunchDaemon, netcfg helper, the kext
build/     compiler output: build/client/RTW88Client + per-target *.o (git-ignored)
```

## Development

`dev-reload.sh` rebuilds both targets, reinstalls + reloads the kext into
`/Library/Extensions`, and (optionally) runs the client — the fast inner loop while
iterating on the driver:

```sh
sudo bash dev-reload.sh                       # rebuild + reload kext
sudo bash dev-reload.sh --kscan               # …then run a command
CLIENT_ONLY=1 bash dev-reload.sh --kstats     # skip the kext rebuild/reload
```

`server/load.sh` is the one-shot build → sign → load for the kext alone.

## Future plans

The remaining limitations are about durability and throughput, now that the full
driver is in-kernel:

- **Durability** — handle EAPOL during GTK rekey (re-derive the group key), add a
  beacon-miss link watchdog + auto-reconnect, then roaming.
- **Throughput** — a per-TID RX reorder buffer (so out-of-order A-MPDU retries stop
  ceilinging TCP) plus rate-control feedback and 40/80 MHz channel bonding.
- **Native UI (optional)** — an IO80211 front-end over the in-kernel MAC for native
  Wi-Fi UI / HeliPort, quarantined as a front-end so the private API never touches
  the core driver.

## Credits & license

Radio bring-up is lifted from the Linux **rtw88** driver (Realtek/kernel
authors). See [LICENSE](LICENSE). This is an independent hobbyist project, not
affiliated with Realtek or Apple. Hardware needed: an RTL8821CE on an Intel
Mac / Hackintosh booted without an IOMMU (`npci=0x2000`, `DisableIoMapper`).
