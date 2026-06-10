# RTW88 on OpenCore — boot setup for auto-connect WiFi

This makes the RTL8821CE come up and connect to your WiFi automatically on every
boot: OpenCore injects the **RTW88Server** kext (the driver), and the **rtwd**
LaunchDaemon joins your saved networks — routing the whole system through the card.

Two moving parts:

| Part | What it is | Loaded by |
|------|------------|-----------|
| `RTW88Server.kext` | the driver: maps the card's MMIO, owns DMA, and runs the full 802.11 stack in-kernel (power/fw/PHY/scan/assoc/WPA2/CCMP + TX-RX) behind a macOS `enX` + a control user-client | OpenCore (or `/Library/Extensions`) |
| `rtwd` | the control daemon + CLI: tells the kext to scan/connect, brings `enX` online (configd DHCP), auto-reconnects, and serves the menu-bar app / CLI | a LaunchDaemon at boot |

---

## 0. Prerequisite — NO IOMMU (required)

This driver's safety model assumes there is **no** IOMMU: the kext owns all DMA
addressing and programs the device with raw physical addresses. You **must** run
the machine that way or DMA will be remapped/trapped and the card won't work:

1. **Boot-arg** `npci=0x2000` — in OpenCore `config.plist` → `NVRAM` →
   `7C436110-AB2A-4BBB-A880-FE41995C9F82` → `boot-args` (append, space-separated).
2. **Quirk** `DisableIoMapper = true` — `config.plist` → `Kernel` → `Quirks`.

(If you already use these for other reasons, nothing to change.)

---

## 1. Build + sign the kext

```sh
bash server/build.sh
codesign --force --sign - dist/RTW88Server.kext
```

This produces `dist/RTW88Server.kext` (ad-hoc signed; fine for a local
machine — OpenCore doesn't require a Developer ID).

---

## 2. Inject the kext via OpenCore

Copy the kext into your EFI and add it to `config.plist`:

```sh
cp -R dist/RTW88Server.kext /Volumes/EFI/EFI/OC/Kexts/
```

`config.plist` → `Kernel` → `Add`, append:

```xml
<dict>
    <key>Arch</key>            <string>x86_64</string>
    <key>BundlePath</key>      <string>RTW88Server.kext</string>
    <key>Comment</key>         <string>RTL8821CE WiFi driver</string>
    <key>Enabled</key>         <true/>
    <key>ExecutablePath</key>  <string>Contents/MacOS/RTW88Server</string>
    <key>MaxKernel</key>       <string></string>
    <key>MinKernel</key>       <string></string>
    <key>PlistPath</key>       <string>Contents/Info.plist</string>
</dict>
```

Reboot. Verify it matched the card:

```sh
ioreg -c RTW88Server | grep -i rtw88         # should list the service
kextstat | grep rtw88                        # com.rtw88.RTW88Server loaded
```

> Alternative (no OpenCore edit): `sudo bash dist/install.sh` instead copies the
> kext to `/Library/Extensions`. The first load there needs a one-time approval
> in **System Settings → Privacy & Security** + a reboot. OpenCore injection
> skips the approval prompt.

---

## 3. Install the daemon (+ optional menu-bar app)

```sh
sudo bash dist/install.sh           # daemon + CLI
sudo bash dist/install.sh --gui     # ... plus the menu-bar app
```

This installs `rtwd` to `/usr/local/bin`, the netcfg helper to
`/usr/local/libexec`, a config file to `/usr/local/etc/rtw88.conf` (0600), and
the LaunchDaemon `com.rtw88.rtwd` (started immediately).

Join a network once — its password is saved and reused on every boot:

```sh
rtwd connect "MyHomeWiFi" "home-pass"
```

or pre-list several networks in `/usr/local/etc/rtw88.conf` (it joins whichever
is strongest in range):

```ini
network  = MyHomeWiFi
password = home-pass
network  = Phone Hotspot
password = hotspot-pass
band     = both
```

Within a few seconds of boot you should have a default route + DNS through the
card — `ping 8.8.8.8`, `curl https://example.com`, and Safari all work.

To stop / disable:

```sh
sudo launchctl bootout system/com.rtw88.rtwd
```

To remove everything: `sudo bash dist/uninstall.sh` (plus delete the kext from
`EFI/OC/Kexts` and its `config.plist` entry if you injected it via OpenCore).

---

## Troubleshooting

- **`RTW88Server not found`** — the kext didn't load: check step 0 (boot-arg +
  quirk), that the card is an RTL8821CE (`0xc82110ec`), and `kextstat | grep rtw88`.
- **Connects but no internet** — check `/var/log/rtw88d.log` for the enX/DHCP
  line. `ifconfig` should show the kext's `enX` with a leased IP, and
  `route -n get 8.8.8.8` should go via it.
- **Wrong/no network joined** — the SSID in `rtw88.conf` must match exactly
  (case-sensitive); run `rtwd scan` to see what's visible.
- It is normal to see the link renegotiate if you roam; rtwd's auto-connect
  rejoins the strongest known network after a drop.
