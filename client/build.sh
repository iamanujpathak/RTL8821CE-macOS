#!/bin/bash
# Build RTW88Client — a thin userspace control utility for the RTW88Server kext.
# It only talks to the kext over the user-client ABI (no radio logic of its own),
# so it compiles just three files. Plain userspace clang, no kernel headers.
# Objects -> build/client/*.o, binary -> build/client/RTW88Client.
set -e
cd "$(dirname "$0")/.."          # repo root
OUT=build/client
mkdir -p "$OUT"

# -Icore: shared rtw_hw.h / config.h interfaces (the structs the ABI marshals).
# -arch x86_64: the card is Intel-Mac/Hackintosh only (matches the kext's -arch x86_64),
# so force x86_64 even when building on an Apple-Silicon host / CI runner.
CFLAGS="-arch x86_64 -O0 -g -Wall -Wno-unused-function -std=gnu11 -Icore -Iclient"

SRCS="client/main.c client/config.c client/rtw_hw.c"

OBJS=""
for src in $SRCS; do
  obj="$OUT/$(basename "${src%.c}").o"
  clang $CFLAGS -c "$src" -o "$obj"
  OBJS="$OBJS $obj"
done

clang -arch x86_64 $OBJS -framework IOKit -framework CoreFoundation -o "$OUT/RTW88Client"
echo "built ./$OUT/RTW88Client   (run: sudo ./$OUT/RTW88Client --config rtw88.conf   — RTW88Server kext must be loaded)"

# rtwd — the HeliPort-style control daemon (root LaunchDaemon backing the menu-bar app).
# Reuses the kext ABI via rtw_hw.o (built above); no radio logic of its own.
clang $CFLAGS -c client/rtwd.c -o "$OUT/rtwd.o"
clang -arch x86_64 "$OUT/rtwd.o" "$OUT/rtw_hw.o" -framework IOKit -framework CoreFoundation -o "$OUT/rtwd"
echo "built ./$OUT/rtwd          (run: sudo ./$OUT/rtwd   then: echo scan | nc -U /var/run/rtw88d.sock)"
