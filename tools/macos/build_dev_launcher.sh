#!/bin/zsh
# Build "Fable II Launcher.app" (named "Fable II Dev Launcher.app" before 2026-10-15): the one Fable II app (the
# player app's binary, docs/player-app.md "Developer mode") in its launcher role (Info.plist Fable2DevLauncher),
# this checkout pre-set (Dev Options off by default, 2026-10-19). The way to play from source.
# Kept for tools and agents: bundle id local.fable2recomp.devlauncher (and its defaults domain), executable
# FableIIDevLauncher, this script's name, out/dev-launcher and every CLI flag. --install removes the old-named copy.
#   tools/macos/build_dev_launcher.sh            -> out/dev-launcher/Fable II Launcher.app
#   tools/macos/build_dev_launcher.sh --install  -> also copies it next to the repo
#                                                   (the workspace folder that holds the other launchers)
# The app finds this checkout through the Fable2RepoRoot Info.plist key (override: FABLE2_REPO_ROOT).
set -euo pipefail
REPO=${0:A:h:h:h}
# Builds don't overlap perf runs (local agent docs): run under a build marker that holds the perf lock shared
# (FABLE2_BUILD_ROLE=play, as the Setup app sets, skips the wait). FABLE2_BUILD_HELD is set inside.
if [[ -z "${FABLE2_BUILD_HELD:-}" && -x /usr/bin/python3 ]]; then
  exec /usr/bin/python3 "$REPO/tools/fable2_common.py" build-hold --what "dev launcher" -- /bin/zsh "$0" "$@"
fi
OUT="$REPO/out/dev-launcher"
APP="$OUT/Fable II Launcher.app"
rm -rf "$OUT/Fable II Dev Launcher.app"  # the pre-2026-10-15 name
ICON="$REPO/out/build/mac-arm64-metal-release/Fable II.app/Contents/Resources/FableII.icns"
VERSION_TEXT=$(tr -d '[:space:]' < "$REPO/VERSION.txt" 2>/dev/null || true)
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
xcrun swiftc -O -target arm64-apple-macos15.0 -o "$APP/Contents/MacOS/FableIIDevLauncher" \
  "$REPO/tools/macos/Player/main.swift" "$REPO"/tools/macos/DevLauncher/*.swift \
  "$REPO"/tools/macos/Shared/*.swift
if [[ -f "$ICON" ]]; then cp "$ICON" "$APP/Contents/Resources/FableII.icns"
else echo "warning: game icon not found ($ICON); build the game first" >&2; fi
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>FableIIDevLauncher</string>
  <key>CFBundleIdentifier</key><string>local.fable2recomp.devlauncher</string>
  <key>CFBundleName</key><string>Fable II Launcher</string>
  <key>CFBundleDisplayName</key><string>Fable II Launcher</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>${VERSION_TEXT:-0.0.0}</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>CFBundleIconFile</key><string>FableII</string>
  <key>LSMinimumSystemVersion</key><string>15.0</string>
  <key>LSMultipleInstancesProhibited</key><true/>
  <key>NSHighResolutionCapable</key><true/>
  <key>Fable2RepoRoot</key><string>$REPO</string>
  <key>Fable2DevLauncher</key><true/>
</dict>
</plist>
PLIST
codesign --force --sign - "$APP" >/dev/null
echo "Built: $APP"
if [[ "${1:-}" == "--install" ]]; then
  DEST="${REPO:h}/Fable II Launcher.app"
  rm -rf "$DEST"
  cp -R "$APP" "$DEST"
  echo "Installed: $DEST"
  OLD="${REPO:h}/Fable II Dev Launcher.app"  # the pre-2026-10-15 name: same bundle id, so remove it
  if [[ -d "$OLD" ]]; then rm -rf "$OLD"; echo "Removed the old-named copy: $OLD"; fi
fi
