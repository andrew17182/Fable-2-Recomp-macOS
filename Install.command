#!/bin/bash
# Fable II for macOS: first-run setup. Double-click this file in Finder.
# It builds the "Fable II Setup" app into out/setup/ and opens it; the app asks for your
# Fable II disc image, extracts the game files and builds everything.
# Arguments are passed to the app, e.g. ./Install.command --iso "My Disc.iso" --auto
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
APP="$ROOT/out/setup/Fable II Setup.app"
BIN="$APP/Contents/MacOS/FableIISetup"
SOURCES=("$ROOT"/tools/macos/Setup/*.swift)

say() { printf '%s\n' "$*"; }
pause_exit() {
    # Keep the Terminal window readable when launched from Finder.
    [[ -t 0 ]] && read -r -p "Press Return to close this window." _ || true
    exit "$1"
}

say "Fable II Setup"
say "Project folder: $ROOT"
say ""

if [[ "$(uname -s)" != Darwin || "$(sysctl -n hw.optional.arm64 2>/dev/null || echo 0)" != 1 ]]; then
    say "This port needs a Mac with Apple Silicon (M1 or later)."
    pause_exit 1
fi

# Xcode command-line tools (they provide swiftc, clang, python3 and git).
if ! xcode-select -p >/dev/null 2>&1 || ! xcrun --find swiftc >/dev/null 2>&1; then
    say "The Xcode command-line tools are not installed. They are needed to build the setup app."
    say "Apple's installer can install them now (free, a few minutes)."
    if [[ -t 0 ]]; then
        read -r -p "Open Apple's installer now? [Y/n] " answer
        if [[ -z "$answer" || "$answer" =~ ^[Yy] ]]; then
            xcode-select --install || true
            say ""
            say "When Apple's installer has finished, double-click Install.command again."
        fi
    else
        say "Run: xcode-select --install"
    fi
    pause_exit 1
fi

# Build the setup app when it is missing or older than its sources.
needs_build=0
[[ -x "$BIN" ]] || needs_build=1
if [[ $needs_build == 0 ]]; then
    for source in "${SOURCES[@]}" "$0"; do
        [[ "$source" -nt "$BIN" ]] && needs_build=1
    done
fi
if [[ $needs_build == 1 ]]; then
    say "Building the setup app…"
    rm -rf "$APP"
    mkdir -p "$APP/Contents/MacOS"
    if ! xcrun swiftc -O -swift-version 5 -parse-as-library -target arm64-apple-macos14.0 \
            -o "$BIN" "${SOURCES[@]}"; then
        say "Could not build the setup app (see the errors above)."
        pause_exit 1
    fi
    # Escape the path for the plist (it may contain spaces, & or <).
    root_xml="$(printf '%s' "$ROOT" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g')"
    cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>FableIISetup</string>
  <key>CFBundleIdentifier</key><string>local.fable2recomp.setup</string>
  <key>CFBundleName</key><string>Fable II Setup</string>
  <key>CFBundleDisplayName</key><string>Fable II Setup</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>Fable2RepoRoot</key><string>$root_xml</string>
</dict>
</plist>
PLIST
    codesign --force --sign - "$APP" >/dev/null 2>&1 || true
fi

if [[ $# -gt 0 ]]; then
    # Unattended/test runs: run in this terminal so the output and exit status are visible.
    exec "$BIN" "$@"
fi
say "Opening Fable II Setup…"
open "$APP"
