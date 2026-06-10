#!/bin/bash
# install.sh — build + install RTW88 (RTL8821CE WiFi) from source.
#
#   sudo bash dist/install.sh           # kext + rtwd daemon (auto-connects from config)
#   sudo bash dist/install.sh --gui     # ... plus the menu-bar app (pick networks in the UI)
#
# Always installs: RTW88Server.kext (the driver, loads at boot), the unified
# `rtwd` binary (daemon + CLI), the netcfg helper, a config file, and the rtwd
# LaunchDaemon. rtwd auto-connects to the strongest saved network on boot and
# keeps the link up; `rtwd connect/scan/status` drives it from a terminal.
# --gui additionally installs RTW88Menu.app + its LaunchAgent.
#
# For an OpenCore-injected kext instead, see dist/OpenCore.md.
# To remove everything: sudo bash dist/uninstall.sh
set -e
cd "$(dirname "$0")/.."
RUSER="${SUDO_USER:-root}"

GUI=0
for a in "$@"; do [ "$a" = "--gui" ] && GUI=1; done

[ "$(id -u)" = "0" ] || { echo "run with sudo: sudo bash dist/install.sh [--gui]"; exit 1; }

echo "==> build rtwd (daemon + CLI)"
sudo -u "$RUSER" bash client/build.sh
install -m 755 build/client/rtwd /usr/local/bin/rtwd
install -d /usr/local/libexec
install -m 755 dist/rtw88-eth-netcfg.sh /usr/local/libexec/rtw88-eth-netcfg.sh
# retire the pre-merge split binary + its LaunchDaemon if present
launchctl bootout system/com.rtw88.client 2>/dev/null || true
rm -f /Library/LaunchDaemons/com.rtw88.client.plist /usr/local/bin/RTW88Client

echo "==> build + install RTW88Server.kext to /Library/Extensions (auto-loads at boot)"
sudo -u "$RUSER" bash server/build.sh
sudo -u "$RUSER" codesign --force --sign - dist/RTW88Server.kext
rm -rf /Library/Extensions/RTW88Server.kext
cp -R dist/RTW88Server.kext /Library/Extensions/
chown -R root:wheel /Library/Extensions/RTW88Server.kext
chmod -R 755 /Library/Extensions/RTW88Server.kext
kmutil load -p /Library/Extensions/RTW88Server.kext 2>/dev/null || \
    echo "    (kext will load on next boot; first install needs approval + reboot)"

echo "==> install the config file (rtwd appends networks you join; 0600 — it holds passphrases)"
if [ ! -f /usr/local/etc/rtw88.conf ]; then
	mkdir -p /usr/local/etc
	cp client/rtw88.conf.example /usr/local/etc/rtw88.conf
	echo "    wrote /usr/local/etc/rtw88.conf  (optionally pre-list your network/password)"
else
	echo "    kept existing /usr/local/etc/rtw88.conf"
fi
chmod 600 /usr/local/etc/rtw88.conf

echo "==> install + (re)start the rtwd LaunchDaemon"
launchctl bootout system/com.rtw88.rtwd 2>/dev/null || true
cp dist/com.rtw88.rtwd.plist /Library/LaunchDaemons/
chown root:wheel /Library/LaunchDaemons/com.rtw88.rtwd.plist
chmod 644 /Library/LaunchDaemons/com.rtw88.rtwd.plist
launchctl bootstrap system /Library/LaunchDaemons/com.rtw88.rtwd.plist 2>/dev/null || true

if [ "$GUI" = "1" ]; then
	echo "==> GUI: build + install the menu-bar app + its LaunchAgent"
	sudo -u "$RUSER" bash client/gui/build-app.sh
	rm -rf /Applications/RTW88Menu.app
	cp -R build/client/RTW88Menu.app /Applications/
	chown -R "$RUSER" /Applications/RTW88Menu.app

	cp dist/com.rtw88.menu.plist /Library/LaunchAgents/
	chown root:wheel /Library/LaunchAgents/com.rtw88.menu.plist
	chmod 644 /Library/LaunchAgents/com.rtw88.menu.plist

	UID_RUSER="$(id -u "$RUSER")"
	launchctl bootout    "gui/$UID_RUSER/com.rtw88.menu" 2>/dev/null || true
	launchctl bootstrap  "gui/$UID_RUSER" /Library/LaunchAgents/com.rtw88.menu.plist 2>/dev/null || true
fi

cat <<EOF

Done. Next:
  1. First-ever kext install: approve "RTW88Server" in
     System Settings > Privacy & Security, then reboot once.
  2. Join a network:
       GUI:  click the Wi-Fi icon in the menu bar (if installed with --gui)
       CLI:  rtwd connect "MySSID" "my-password"
     The password is saved to /usr/local/etc/rtw88.conf (0600) and the daemon
     auto-reconnects on every boot.
  Status:        rtwd status            Logs:  tail -f /var/log/rtw88d.log
  Stop daemon:   sudo launchctl bootout system/com.rtw88.rtwd
  Uninstall:     sudo bash dist/uninstall.sh
EOF
