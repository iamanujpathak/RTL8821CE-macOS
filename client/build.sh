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
CFLAGS="-O0 -g -Wall -Wno-unused-function -std=gnu11 -Icore -Iclient"

SRCS="client/main.c client/config.c client/rtw_hw.c"

OBJS=""
for src in $SRCS; do
  obj="$OUT/$(basename "${src%.c}").o"
  clang $CFLAGS -c "$src" -o "$obj"
  OBJS="$OBJS $obj"
done

clang $OBJS -framework IOKit -framework CoreFoundation -o "$OUT/RTW88Client"
echo "built ./$OUT/RTW88Client   (run: sudo ./$OUT/RTW88Client --config rtw88.conf   — RTW88Server kext must be loaded)"
