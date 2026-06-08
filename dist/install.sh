#!/bin/bash
# install.sh — install RTW88 (RTL8821CE WiFi) for seamless, auto-on-boot net.
#
#   sudo bash dist/install.sh
#
# Installs: the RTW88Server kext (the driver, loads at boot), the RTW88Client
# control utility, the netcfg helper, a config file, and a LaunchDaemon that
# connects at boot and keeps the link up. For an OpenCore-injected kext instead,
# see dist/OpenCore.md.
set -e
cd "$(dirname "$0")/.."
RUSER="${SUDO_USER:-root}"

# --gui: install the HeliPort-style menu-bar app + rtwd daemon (pick networks from
# the GUI) INSTEAD of the auto-connect-from-config LaunchDaemon. The two are mutually
# exclusive — both would contend for the kext.
GUI=0
for a in "$@"; do [ "$a" = "--gui" ] && GUI=1; done

[ "$(id -u)" = "0" ] || { echo "run with sudo: sudo bash dist/install.sh"; exit 1; }

echo "==> build RTW88Client + rtwd (control utility + daemon)"
sudo -u "$RUSER" bash client/build.sh
install -m 755 build/client/RTW88Client /usr/local/bin/RTW88Client
install -m 755 build/client/rtwd         /usr/local/bin/rtwd
install -d /usr/local/libexec
install -m 755 dist/rtw88-eth-netcfg.sh  /usr/local/libexec/rtw88-eth-netcfg.sh

echo "==> build + install RTW88Server.kext to /Library/Extensions (auto-loads at boot)"
sudo -u "$RUSER" bash server/build.sh
sudo -u "$RUSER" codesign --force --sign - dist/RTW88Server.kext || true
rm -rf /Library/Extensions/RTW88Server.kext
cp -R dist/RTW88Server.kext /Library/Extensions/
chown -R root:wheel /Library/Extensions/RTW88Server.kext
chmod -R 755 /Library/Extensions/RTW88Server.kext
kmutil load -p /Library/Extensions/RTW88Server.kext 2>/dev/null || \
    echo "    (kext will load on next boot; first install needs approval + reboot)"

if [ "$GUI" = "1" ]; then
	echo "==> GUI mode: build + install the menu-bar app and the rtwd daemon"
	sudo -u "$RUSER" bash client/gui/build-app.sh
	rm -rf /Applications/RTW88Menu.app
	cp -R build/client/RTW88Menu.app /Applications/
	chown -R "$RUSER" /Applications/RTW88Menu.app

	# config file: rtwd reads known networks from here and saves new ones the GUI
	# connects to (so they reconnect password-free + auto-connect on boot).
	if [ ! -f /usr/local/etc/rtw88.conf ]; then
		mkdir -p /usr/local/etc
		cp client/rtw88.conf.example /usr/local/etc/rtw88.conf
		chmod 600 /usr/local/etc/rtw88.conf
		echo "    wrote /usr/local/etc/rtw88.conf  (rtwd will append networks you join)"
	else
		echo "    kept existing /usr/local/etc/rtw88.conf"
	fi

	# rtwd LaunchDaemon (NOT the auto-connect client one)
	launchctl bootout system/com.rtw88.client 2>/dev/null || true
	rm -f /Library/LaunchDaemons/com.rtw88.client.plist
	cp dist/com.rtw88.rtwd.plist /Library/LaunchDaemons/
	chown root:wheel /Library/LaunchDaemons/com.rtw88.rtwd.plist
	chmod 644 /Library/LaunchDaemons/com.rtw88.rtwd.plist

	# menu-bar app LaunchAgent — starts the GUI at login + keeps it alive (no Quit)
	cp dist/com.rtw88.menu.plist /Library/LaunchAgents/
	chown root:wheel /Library/LaunchAgents/com.rtw88.menu.plist
	chmod 644 /Library/LaunchAgents/com.rtw88.menu.plist

	# load both now, in the right domains (daemon = system, agent = the user's GUI)
	UID_RUSER="$(id -u "$RUSER")"
	launchctl bootstrap system /Library/LaunchDaemons/com.rtw88.rtwd.plist 2>/dev/null || true
	launchctl bootout    "gui/$UID_RUSER/com.rtw88.menu" 2>/dev/null || true
	launchctl bootstrap  "gui/$UID_RUSER" /Library/LaunchAgents/com.rtw88.menu.plist 2>/dev/null || true

	cat <<EOF

Done (GUI mode). The rtwd daemon and the menu-bar app are installed and started,
and both auto-start on boot/login.
  1. First-ever kext install: approve "RTW88Server" in
     System Settings > Privacy & Security, then reboot once.
  2. rtwd auto-connects to the strongest saved network. Join a new one from the
     menu (Wi-Fi icon) once; its password is saved to /usr/local/etc/rtw88.conf
     and reused automatically afterwards.
  Logs:   tail -f /var/log/rtw88d.log
  Stop daemon:   sudo launchctl bootout system/com.rtw88.rtwd
  Stop the menu: sudo launchctl bootout gui/$UID_RUSER/com.rtw88.menu
EOF
	exit 0
fi

echo "==> install the config file (edit it with your network(s))"
if [ ! -f /usr/local/etc/rtw88.conf ]; then
	mkdir -p /usr/local/etc
	cp client/rtw88.conf.example /usr/local/etc/rtw88.conf
	echo "    wrote /usr/local/etc/rtw88.conf  <-- EDIT THIS (network/password)"
else
	echo "    kept existing /usr/local/etc/rtw88.conf"
fi

echo "==> install the boot LaunchDaemon"
cp dist/com.rtw88.client.plist /Library/LaunchDaemons/
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
  To stop / disable:
        sudo launchctl bootout system/com.rtw88.client
  (Prefer a GUI? Re-run:  sudo bash dist/install.sh --gui)
EOF
