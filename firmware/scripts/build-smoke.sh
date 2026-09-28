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
