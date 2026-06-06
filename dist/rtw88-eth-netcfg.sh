#!/bin/bash
# rtw88-eth-netcfg.sh — make a kext-created enX a first-class macOS network
# service so scoped routing actually sends internet traffic through it.
#
#   rtw88-eth-netcfg.sh <iface> dhcp                     (let configd DHCP it)
#   rtw88-eth-netcfg.sh <iface> <ip> <netmask> <gw> [dns]   (static)
#
# Why: macOS scoped routing (net.inet.ip.scopedroute=1) binds outbound traffic
# to the PRIMARY SystemConfiguration service's interface. An interface that
# isn't a registered service has no primary status, so a hand-added default
# route is ignored for internet unicast (TCP/ICMP) while ARP/DHCP/multicast
# still work — the exact "local works, internet doesn't" symptom. So we register
# the interface as a service, make it primary, and let configd run DHCP (or set
# it static) — configd then installs the scoped default route + DNS itself.
set -u
IFACE="$1"; MODE="$2"; MASK="${3:-}"; GW="${4:-}"; DNS="${5:-}"
SVCNAME="RTL8821CE"

ifconfig "$IFACE" up 2>/dev/null     # ensure the link is up for configd/DHCP

# Find a service already bound to this device, else create one for its port.
svc_for_device() {
  networksetup -listnetworkserviceorder | awk -v d="$1" '
    /^\([0-9]+\)/ { name=$0; sub(/^\([0-9]+\) /,"",name) }
    $0 ~ ("Device: " d ")") { print name; exit }'
}
port_for_device() {
  networksetup -listallhardwareports | awk -v d="$1" '
    /^Hardware Port:/ { hp=$0; sub(/^Hardware Port: /,"",hp) }
    $0 ~ ("^Device: " d "$") { print hp; exit }'
}

SVC="$(svc_for_device "$IFACE")"
if [ -z "$SVC" ]; then
  PORT="$(port_for_device "$IFACE")"
  if [ -n "$PORT" ] && networksetup -createnetworkservice "$SVCNAME" "$PORT" 2>/dev/null; then
    SVC="$SVCNAME"
  fi
fi

if [ -n "$SVC" ]; then
  if [ "$MODE" = "dhcp" ]; then
    # Let configd own the lease (robust retry) — it installs route+DNS itself.
    networksetup -setdhcp "$SVC"
  else
    networksetup -setmanual "$SVC" "$MODE" "$MASK" "$GW"   # MODE holds the IP
    [ -n "$DNS" ] && networksetup -setdnsservers "$SVC" "$DNS"
  fi
  # Make it the PRIMARY service so the scoped default route binds to this iface.
  # ordernetworkservices needs EVERY service as a separate (possibly space-
  # containing) argument, primary first — build an array (bash 3.2 safe).
  ORDER=("$SVC")
  while IFS= read -r s; do
    s="${s#\*}"                       # strip leading '*' (disabled marker)
    [ -n "$s" ] && [ "$s" != "$SVC" ] && ORDER+=("$s")
  done < <(networksetup -listallnetworkservices | tail -n +2)
  if networksetup -ordernetworkservices "${ORDER[@]}"; then
    # Kick a fresh lease NOW (the interface was just (re)created; don't wait for
    # configd to notice on its own — that's the intermittent "no lease" cause).
    [ "$MODE" = "dhcp" ] && ipconfig set "$IFACE" DHCP 2>/dev/null
    echo "netcfg: service '$SVC' -> $IFACE  mode=$MODE (PRIMARY)"
  else
    echo "netcfg: service '$SVC' configured ($MODE) but could not be made primary"
  fi
  exit 0
fi

echo "netcfg: WARNING no SystemConfiguration service for $IFACE — internet routing"
echo "        needs a service; add one in System Settings > Network, device $IFACE."
exit 1
