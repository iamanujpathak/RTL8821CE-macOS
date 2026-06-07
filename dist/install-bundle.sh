#!/bin/bash
# install-bundle.sh — installer for the PREBUILT release bundle (no compiler needed).
# Ships inside RTW88-macOS.zip alongside the built binaries. Run from the unzipped
# bundle directory:
#
#   sudo bash install.sh          # headless: auto-connect from /usr/local/etc/rtw88.conf
#   sudo bash install.sh --gui    # GUI: menu-bar app + rtwd daemon (pick networks in the UI)
#
# (The repo's dist/install.sh is the build-from-source equivalent.)
set -e
cd "$(dirname "$0")"
RUSER="${SUDO_USER:-root}"

GUI=0
for a in "$@"; do [ "$a" = "--gui" ] && GUI=1; done

[ "$(id -u)" = "0" ] || { echo "run with sudo: sudo bash install.sh [--gui]"; exit 1; }
[ -d RTW88Server.kext ] || { echo "RTW88Server.kext not found next to this script — run inside the unzipped bundle"; exit 1; }

echo "==> install control utilities"
install -m 755 RTW88Client /usr/local/bin/RTW88Client
install -m 755 rtwd         /usr/local/bin/rtwd
install -d /usr/local/libexec
install -m 755 rtw88-eth-netcfg.sh /usr/local/libexec/rtw88-eth-netcfg.sh

echo "==> install RTW88Server.kext to /Library/Extensions (auto-loads at boot)"
rm -rf /Library/Extensions/RTW88Server.kext
cp -R RTW88Server.kext /Library/Extensions/
chown -R root:wheel /Library/Extensions/RTW88Server.kext
chmod -R 755 /Library/Extensions/RTW88Server.kext
kmutil load -p /Library/Extensions/RTW88Server.kext 2>/dev/null || \
    echo "    (kext will load on next boot; first install needs approval + reboot)"

if [ "$GUI" = "1" ]; then
	echo "==> GUI mode: install menu-bar app + rtwd daemon"
	rm -rf /Applications/RTW88Menu.app
	cp -R RTW88Menu.app /Applications/
	chown -R "$RUSER" /Applications/RTW88Menu.app

	launchctl bootout system/com.rtw88.client 2>/dev/null || true
	rm -f /Library/LaunchDaemons/com.rtw88.client.plist
	cp com.rtw88.rtwd.plist /Library/LaunchDaemons/
	chown root:wheel /Library/LaunchDaemons/com.rtw88.rtwd.plist
	chmod 644 /Library/LaunchDaemons/com.rtw88.rtwd.plist

	cat <<EOF

Done (GUI mode). Next:
  1. First-ever kext install: approve "RTW88Server" in
     System Settings > Privacy & Security, then reboot once.
  2. Start the daemon (or it auto-starts on boot):
        sudo launchctl bootstrap system /Library/LaunchDaemons/com.rtw88.rtwd.plist
  3. Launch the app:  open /Applications/RTW88Menu.app   (add to Login Items to persist)
     Click the Wi-Fi icon to scan / connect.  Stop: sudo launchctl bootout system/com.rtw88.rtwd
EOF
	exit 0
fi

echo "==> install the config file (edit it with your network(s))"
if [ ! -f /usr/local/etc/rtw88.conf ]; then
	mkdir -p /usr/local/etc
	cp rtw88.conf.example /usr/local/etc/rtw88.conf
	echo "    wrote /usr/local/etc/rtw88.conf  <-- EDIT THIS (network/password)"
else
	echo "    kept existing /usr/local/etc/rtw88.conf"
fi

echo "==> install the boot LaunchDaemon"
cp com.rtw88.client.plist /Library/LaunchDaemons/
chown root:wheel /Library/LaunchDaemons/com.rtw88.client.plist
chmod 644 /Library/LaunchDaemons/com.rtw88.client.plist

cat <<EOF

Done. Next:
  1. Edit your network(s) into:   /usr/local/etc/rtw88.conf
  2. First-ever kext install: approve "RTW88Server" in
     System Settings > Privacy & Security, then reboot once.
  3. Start now (or it auto-starts on boot):
        sudo launchctl bootstrap system /Library/LaunchDaemons/com.rtw88.client.plist
     Watch:  tail -f /var/log/rtw88.log
  (Prefer a GUI? Re-run:  sudo bash install.sh --gui)
EOF
