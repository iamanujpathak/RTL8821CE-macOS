#!/bin/bash
# Build the RTW88Server.kext (thin user-client bridge). Hand-rolled clang link.
# Objects -> build/server/*.o, kext bundle -> dist/RTW88Server.kext.
set -e
cd "$(dirname "$0")/.."          # repo root

SDK=$(xcrun --show-sdk-path)
KH="$SDK/System/Library/Frameworks/Kernel.framework/Versions/A/Headers"
OUT=build/server
KEXT=dist/RTW88Server.kext
BIN="$KEXT/Contents/MacOS/RTW88Server"

mkdir -p "$OUT" "$KEXT/Contents/MacOS"
cp server/Info.plist "$KEXT/Contents/Info.plist"

CXXFLAGS="-arch x86_64 -fno-exceptions -fno-rtti -fno-builtin -fno-common -mkernel \
  -nostdinc -I$KH -Iserver -isysroot $SDK \
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -Wall \
  -Wno-inconsistent-missing-override -Wno-deprecated-declarations"
# ^ both suppressed warnings originate in Apple's Kernel.framework headers:
#   - inconsistent-missing-override: IOPCIDevice/IORegistryEntry declare overrides
#     without the 'override' keyword.
#   - deprecated-declarations: IOPCIDevice itself is marked "Use PCIDriverKit", but
#     PCIDriverKit is dext-only — a kext has no alternative for raw BAR/MSI access.

clang++ $CXXFLAGS -x c++ -std=c++17 -c server/RTW88Server.cpp -o "$OUT/RTW88Server.o"
clang   $CXXFLAGS -x c   -std=gnu11 -c server/kmod_info.c     -o "$OUT/kmod_info.o"

# The in-kernel control C — kctl.c (MMIO shim + entry points) and the bring-up files
# shared with the client (core/). They include core/rtw_shim.h, so add -Icore. The
# shim's #ifdef KERNEL path keeps them honest in kernel context.
# -D_FORTIFY_SOURCE=0: XNU's <string.h> redefines strcpy/strcat as 3-arg fortify
# macros (__builtin___strcpy_chk); the shared C calls the plain 2-arg form.
# -Wno-sign-conversion/-shadow/-format-extra-args: cosmetic warnings in the
# verbatim-upstream C we reuse unmodified, so silence here rather than edit it.
CCTL="$CXXFLAGS -Icore -Icore/upstream -D_FORTIFY_SOURCE=0 -Wno-unused-function -Wno-shorten-64-to-32 -Wno-implicit-int-conversion -Wno-sign-conversion -Wno-shadow -Wno-format-extra-args"
CTL_OBJS="$OUT/kctl.o $OUT/crypto.o"
clang $CCTL -x c -std=gnu11 -c server/kctl.c          -o "$OUT/kctl.o"
# bundled WPA2 crypto (SHA1/HMAC/PBKDF2/AES) — wpa.c calls these in-kernel
clang $CCTL -x c -std=gnu11 -c crypto/rtw_crypto.c    -o "$OUT/crypto.o"
# bring-up + connect: power-on, TRX rings, fw download, mac_init, efuse, phy_set_param,
# fw H2C handshake, BT-coex antenna, set-channel, scan, auth/assoc, WPA2, key install,
# media-connect.
for f in pwr trx fwdownload macinit efuse phy h2c coex channel scan \
         assoc wpa sec media; do
  clang $CCTL -x c -std=gnu11 -c core/$f.c -o "$OUT/$f.o"
  CTL_OBJS="$CTL_OBJS $OUT/$f.o"
done
# phy/BB/AGC/RF/MAC parameter tables + the firmware blob
clang $CCTL -x c -std=gnu11 -c core/upstream/rtw8821c_table.c -o "$OUT/rtw8821c_table.o"
clang $CCTL -x c -std=gnu11 -c core/fw_blob.c                 -o "$OUT/fw_blob.o"
CTL_OBJS="$CTL_OBJS $OUT/rtw8821c_table.o $OUT/fw_blob.o"

clang++ -arch x86_64 -nostdlib -Xlinker -kext \
  -Wl,-export_dynamic \
  "$OUT/RTW88Server.o" "$OUT/kmod_info.o" $CTL_OBJS \
  -L"$SDK/usr/lib" -lkmodc++ -lkmod \
  -o "$BIN"

echo "built $BIN"
file "$BIN"
echo "--- kextlibs (symbol deps; undefined = resolved against kernel at load) ---"
kextlibs -xml "$KEXT" 2>&1 || true
