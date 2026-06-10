#!/bin/bash
# Build RTW88Menu.app — the HeliPort-style menu-bar GUI (talks to rtwd over the
# Unix socket). No Xcode project: swiftc + a hand-assembled .app bundle.
set -e
cd "$(dirname "$0")/../.."        # repo root
OUT=build/client
APP="$OUT/RTW88Menu.app"
mkdir -p "$APP/Contents/MacOS"

# compile (parse-as-library so @main is honored; needs macOS 13+)
swiftc -O -parse-as-library \
    -target x86_64-apple-macos13.0 \
    client/gui/RTW88Menu.swift \
    -o "$APP/Contents/MacOS/RTW88Menu"

cp client/gui/Info.plist "$APP/Contents/Info.plist"

# ad-hoc sign (Hackintosh; no Developer ID needed). Fail LOUDLY — a silently
# unsigned bundle behaves confusingly under TCC and contradicts the install docs.
codesign --force --sign - "$APP"

echo "built $APP"
echo "run:  open $APP        (needs rtwd running: sudo build/client/rtwd)"
