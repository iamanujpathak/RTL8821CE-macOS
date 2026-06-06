#!/bin/bash
# One-shot: build -> sign -> fix ownership -> load the bridge kext.
# Run with sudo:   sudo bash server/load.sh
#
# First load may prompt once in System Settings > Privacy & Security (new bundle
# id com.rtw88.RTW88Server). Approve + reboot ONCE. After that the cdhash
# never changes (you only edit the userspace app), so it never prompts again.
set -e
cd "$(dirname "$0")/.."          # repo root
KEXT=dist/RTW88Server.kext
RUSER="${SUDO_USER:-$(id -un)}"

echo "==> clean stale build output"
rm -rf "$KEXT" build/server

echo "==> build (as $RUSER)"
sudo -u "$RUSER" bash server/build.sh

echo "==> ad-hoc sign (as $RUSER)"
sudo -u "$RUSER" codesign --force --sign - "$KEXT"

echo "==> chown root:wheel + chmod 755"
chown -R root:wheel "$KEXT"
chmod -R 755 "$KEXT"

echo "==> kmutil load"
kmutil load -p "$KEXT"
echo "==> loaded. Verify:  ioreg -c RTW88Server | grep -i bridge"
