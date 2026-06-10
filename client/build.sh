#!/bin/bash
# Build rtwd — the single userspace binary: control daemon (no args, the
# LaunchDaemon invocation) + CLI client (subcommands over the daemon socket) +
# low-level kext diagnostics (--k*). It only talks to the kext over the
# user-client ABI (no radio logic of its own). Plain userspace clang, no kernel
# headers. Objects -> build/client/*.o, binary -> build/client/rtwd.
set -e
cd "$(dirname "$0")/.."          # repo root
OUT=build/client
mkdir -p "$OUT"

# -Icore: shared rtw_hw.h / config.h interfaces (the structs the ABI marshals).
# -arch x86_64: the card is Intel-Mac/Hackintosh only (matches the kext's -arch x86_64),
# so force x86_64 even when building on an Apple-Silicon host / CI runner.
CFLAGS="-arch x86_64 -O2 -g -Wall -Wno-unused-function -std=gnu11 -Icore -Iclient"

SRCS="client/main.c client/rtwd.c client/config.c client/rtw_hw.c"

OBJS=""
for src in $SRCS; do
  obj="$OUT/$(basename "${src%.c}").o"
  clang $CFLAGS -c "$src" -o "$obj"
  OBJS="$OBJS $obj"
done

clang -arch x86_64 $OBJS -framework IOKit -framework CoreFoundation -o "$OUT/rtwd"
echo "built ./$OUT/rtwd"
echo "  daemon: sudo ./$OUT/rtwd            (or the com.rtw88.rtwd LaunchDaemon)"
echo "  CLI:    ./$OUT/rtwd scan | connect SSID PASS | status | disconnect"
