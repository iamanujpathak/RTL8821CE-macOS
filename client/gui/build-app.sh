#!/bin/bash
# Build RTW88Menu.app — the HeliPort-style menu-bar GUI (talks to rtwd over the
# Unix socket). No Xcode project: swiftc + a hand-assembled .app bundle.
set -e
cd "$(dirname "$0")/../.."        # repo root
OUT=build/client
APP="$OUT/RTW88Menu.app"
mkdir -p "$APP/Contents/MacOS"

# compile (parse-as-library so @main is honored; SwiftUI MenuBarExtra needs macOS 13+)
swiftc -O -parse-as-library \
    -target x86_64-apple-macos13.0 \
    client/gui/RTW88Menu.swift \
    -o "$APP/Contents/MacOS/RTW88Menu"

cp client/gui/Info.plist "$APP/Contents/Info.plist"

# ad-hoc sign so it launches (Hackintosh; no Developer ID needed)
codesign --force --sign - "$APP" 2>/dev/null || true

echo "built $APP"
echo "run:  open $APP        (needs rtwd running: sudo build/client/rtwd)"
