#!/bin/bash
# uninstall.sh — remove everything RTW88 installed (kext, daemon, app, helpers).
#
#   sudo bash dist/uninstall.sh                # keep /usr/local/etc/rtw88.conf
#   sudo bash dist/uninstall.sh --purge        # also remove the config (passwords!)
#
# The kext unload may require a reboot if the driver is busy.
set -u
[ "$(id -u)" = "0" ] || { echo "run with sudo: sudo bash dist/uninstall.sh [--purge]"; exit 1; }

PURGE=0
for a in "$@"; do [ "$a" = "--purge" ] && PURGE=1; done

echo "==> stop + remove the launchd jobs"
launchctl bootout system/com.rtw88.rtwd   2>/dev/null || true
launchctl bootout system/com.rtw88.client 2>/dev/null || true   # pre-merge legacy
for uid in $(ls /Users 2>/dev/null | while read -r u; do id -u "$u" 2>/dev/null; done); do
	launchctl bootout "gui/$uid/com.rtw88.menu" 2>/dev/null || true
done
rm -f /Library/LaunchDaemons/com.rtw88.rtwd.plist \
      /Library/LaunchDaemons/com.rtw88.client.plist \
      /Library/LaunchAgents/com.rtw88.menu.plist

echo "==> remove the binaries + app + helper"
pkill -x RTW88Menu 2>/dev/null || true
rm -f  /usr/local/bin/rtwd /usr/local/bin/RTW88Client
rm -rf /Applications/RTW88Menu.app
rm -f  /usr/local/libexec/rtw88-eth-netcfg.sh
rm -f  /var/db/rtw88.radio-off /var/run/rtw88d.sock
rm -f  /var/log/rtw88d.log /var/log/rtw88.log

echo "==> unload + remove the kext"
kmutil unload -b com.rtw88.RTW88Server 2>/dev/null || \
	echo "    (kext busy or not loaded — it is gone after the next reboot)"
rm -rf /Library/Extensions/RTW88Server.kext

if [ "$PURGE" = "1" ]; then
	echo "==> removing the config (saved networks + passphrases)"
	rm -f /usr/local/etc/rtw88.conf
else
	echo "==> kept /usr/local/etc/rtw88.conf (pass --purge to remove saved passwords)"
fi

echo "Done. If the kext was loaded, reboot to fully release the hardware."
echo "(OpenCore users: also remove RTW88Server.kext from EFI/OC/Kexts + config.plist.)"
