#!/usr/bin/env bash
# Build the app, install it on a connected iPhone, launch it and stream its
# console over the cable.
#
# It builds the Release configuration: the JavaScript is embedded, so the app
# needs no Metro (a Debug bundle cannot run embedded, and this Mac's security
# software blocks the phone from reaching Metro over Wi-Fi). Native logs
# (NSLog, os_log) and console.log from JavaScript all arrive on stdout; lines
# from the app's own tracing start with [Quiesco].
#
# Usage: scripts/device-debug.sh [--no-build] [DEVICE_UDID]
#   --no-build    relaunch and stream without rebuilding
#   DEVICE_UDID   defaults to the first paired physical iPhone

set -euo pipefail
cd "$(dirname "$0")/.."

BUILD=1
if [[ "${1:-}" == "--no-build" ]]; then
  BUILD=0
  shift
fi

DEVICE="${1:-$(xcrun devicectl list devices 2>/dev/null | awk '/physical/ && /iPhone/ {for (i = 1; i <= NF; i++) if ($i ~ /^[0-9A-F]{8}-[0-9A-F]{16}$/) {print $i; exit}}')}"
if [[ -z "$DEVICE" ]]; then
  echo "No paired iPhone found. Connect it with a cable, unlock it and trust this Mac." >&2
  exit 1
fi

TEAM="$(node -p "require('./app.json').expo.ios.appleTeamId")"
BUNDLE_ID="$(node -p "require('./app.json').expo.ios.bundleIdentifier")"
APP="ios/build-device/Build/Products/Release-iphoneos/Quiesco.app"

if [[ "$BUILD" == 1 ]]; then
  echo "› Building Release for $DEVICE (team $TEAM)"
  (cd ios && xcodebuild \
    -workspace Quiesco.xcworkspace -scheme Quiesco -configuration Release \
    -destination "id=$DEVICE" -derivedDataPath build-device \
    -allowProvisioningUpdates DEVELOPMENT_TEAM="$TEAM" CODE_SIGN_STYLE=Automatic \
    build -quiet)
  echo "› Installing"
  xcrun devicectl device install app --device "$DEVICE" "$APP" >/dev/null
fi

echo "› Launching $BUNDLE_ID and streaming its console (Ctrl-C to stop)"
# OS_ACTIVITY_DT_MODE mirrors NSLog/os_log output to stderr, which --console bridges.
exec xcrun devicectl device process launch \
  --device "$DEVICE" --terminate-existing --console \
  --environment-variables '{"OS_ACTIVITY_DT_MODE": "1"}' \
  "$BUNDLE_ID" 2>&1
