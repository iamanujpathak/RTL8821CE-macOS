#!/bin/bash
#
# dev-reload.sh — rebuild RTW88Client + RTW88Server.kext, reload the kext, and run
# the client. The one-command dev cycle for testing kext changes (P2a's --kpoweron,
# and anything after).
#
# Usage:
#   bash dev-reload.sh                 # rebuild+reload, then run: RTW88Client --kpoweron
#   bash dev-reload.sh --config reference/rtw88.conf   # ... run the normal flow instead
#   bash dev-reload.sh --no-run        # rebuild+reload only, don't run the client
#   CLIENT_ONLY=1 bash dev-reload.sh   # skip the kext (client-only changes)
#
# Needs admin rights for the kext install/load — it calls sudo itself, so run it as
# your normal user (it'll prompt for your password once).
set -euo pipefail
cd "$(dirname "$0")"

KEXT_ID=com.rtw88.RTW88Server
LE=/Library/Extensions/RTW88Server.kext

# parse a couple of our own flags; everything else is passed to the client
RUN=1
ARGS=()
for a in "$@"; do
  case "$a" in
    --no-run) RUN=0 ;;
    *) ARGS+=("$a") ;;
  esac
done
[ ${#ARGS[@]} -eq 0 ] && ARGS=(--kpoweron)

echo "==> sudo (cached for the privileged steps)"
sudo -v

echo "==> [1/6] stop auto-connect daemon if running (frees the device)"
sudo launchctl bootout system/com.rtw88.client 2>/dev/null || true

echo "==> [2/6] build RTW88Client"
bash client/build.sh >/dev/null && echo "    client built"

if [ "${CLIENT_ONLY:-0}" != "1" ]; then
  echo "==> [3/6] build RTW88Server.kext (clearing stale root-owned bundle first)"
  sudo rm -rf dist/RTW88Server.kext
  bash server/build.sh >/dev/null && echo "    kext built"

  echo "==> [4/6] install to /Library/Extensions"
  sudo rm -rf "$LE"
  sudo cp -R dist/RTW88Server.kext "$LE"
  sudo chown -R root:wheel "$LE"
  sudo chmod -R 755 "$LE"
  sudo codesign --force --sign - "$LE" >/dev/null 2>&1 || true

  echo "==> [5/6] reload kext"
  sudo kmutil unload -b "$KEXT_ID" 2>/dev/null || true
  sleep 1
  if sudo kmutil load -p "$LE" 2>/tmp/rtw_kmutil.err; then
    echo "    kmutil load OK"
  else
    echo "    kmutil load FAILED:"; sed 's/^/      /' /tmp/rtw_kmutil.err
    echo "    (if it says 'already loaded' or 'busy', a reboot guarantees a clean reload)"
  fi
  sleep 1
  if kextstat 2>/dev/null | grep -q "$KEXT_ID"; then
    echo "    kext loaded ✓  ($(kextstat 2>/dev/null | grep "$KEXT_ID" | awk '{print $6,$7}'))"
  else
    echo "    WARN: $KEXT_ID not in kextstat — load may have failed"
  fi
else
  echo "==> [3-5/6] CLIENT_ONLY=1 — skipping kext rebuild/reload"
fi

if [ "$RUN" = "1" ]; then
  echo "==> [6/6] run: sudo ./build/client/RTW88Client ${ARGS[*]}"
  echo "------------------------------------------------------------"
  sudo ./build/client/RTW88Client "${ARGS[@]}" || true
  echo "------------------------------------------------------------"
  echo "kernel log (RTW88 / scan diag):"
  sudo dmesg 2>/dev/null | grep -iE "RTW88|\[rtw |MSI armed|interrupts serviced|probe-response|TX WORKS|RX ring|antenna|GNT|AUTH resp|ASSOC resp|HANDSHAKE|media-connect|EAPOL|GTK|keys installed|ASSOCIATED" | tail -30 \
    || echo "  (none — use Console.app if dmesg is empty)"
else
  echo "==> [6/6] --no-run: skipping client run"
fi
