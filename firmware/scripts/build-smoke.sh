#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"$root/scripts/arduino-cli.sh" compile \
  --fqbn "Seeeduino:mbed:xiaonRF52840Plus" \
  --warnings all \
  --build-path "$root/.build/smoke-ble" \
  "$root/smoke/ble"

"$root/scripts/arduino-cli.sh" compile \
  --fqbn "Seeeduino:mbed:xiaonRF52840Plus" \
  --warnings all \
  --build-path "$root/.build/smoke-sensors" \
  "$root/smoke/sensors"

"$root/scripts/arduino-cli.sh" compile \
  --fqbn "Seeeduino:mbed:xiaonRF52840Plus" \
  --warnings all \
  --build-path "$root/.build/smoke-rftest" \
  "$root/smoke/rftest"

# Drag-and-drop image of the factory test for the XIAO-BOOT drive, so the
# assembler can flash it without a toolchain (HARDWARE.md §9).
version="$(tr -d '[:space:]' < "$root/VERSION")"
python3 "$root/scripts/make-uf2.py" "$root/.build/smoke-sensors/sensors.ino.bin" \
  "$root/.build/smoke-sensors/quiesco-factory-test-$version.uf2"
echo "UF2: $root/.build/smoke-sensors/quiesco-factory-test-$version.uf2"
