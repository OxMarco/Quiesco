#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fqbn="Seeeduino:mbed:xiaonRF52840Plus"
debug=0
ble_trace=0
trace=0
no_battery=0

for arg in "$@"; do
  case "$arg" in
    --debug) debug=1 ;;
    --ble-trace) ble_trace=1 ;;  # raw HCI trace; needs --debug for the serial port
    --trace) trace=1 ;;  # driver trace (diagnostics/Trace.h); needs --debug
    --no-battery) no_battery=1 ;;  # bench unit without a cell (BuildConfig.h)
    *) echo "unknown option: $arg" >&2; exit 1 ;;
  esac
done

if [ "$trace" -eq 1 ] && [ "$debug" -eq 0 ]; then
  echo "--trace needs --debug: only a debug build opens the serial port." >&2
  exit 1
fi

# VERSION holds MAJOR.MINOR.PATCH; the components reach FirmwareVersion.h as
# plain integers, which survive arduino-cli's build-property quoting.
version="$(tr -d '[:space:]' < "$root/VERSION")"
if [[ ! "$version" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
  echo "VERSION must be MAJOR.MINOR.PATCH, got '$version'." >&2
  exit 1
fi
version_flags="-DQUIESCO_VERSION_MAJOR=${BASH_REMATCH[1]}"
version_flags+=" -DQUIESCO_VERSION_MINOR=${BASH_REMATCH[2]}"
version_flags+=" -DQUIESCO_VERSION_PATCH=${BASH_REMATCH[3]}"

# Release, debug and trace share one build path, and arduino-cli does not
# always recompile when only the flags change: switching variants produced
# images with a mix of flags. Start clean whenever the flags differ.
build_path="$root/.build/quiesco_firmware"
flags="-DQUIESCO_DEBUG=$debug -DQUIESCO_BLE_TRACE=$ble_trace -DQUIESCO_TRACE=$trace -DQUIESCO_NO_BATTERY=$no_battery $version_flags"
stamp="$build_path/.quiesco-flags"
if [ -d "$build_path" ] && [ "$(cat "$stamp" 2>/dev/null)" != "$flags" ]; then
  rm -rf "$build_path"
fi
mkdir -p "$build_path"
printf '%s' "$flags" > "$stamp"

"$root/scripts/arduino-cli.sh" compile \
  --fqbn "$fqbn" \
  --warnings all \
  --build-path "$build_path" \
  --build-property "compiler.cpp.extra_flags=$flags" \
  "$root"
