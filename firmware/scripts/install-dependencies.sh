#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cli="$root/scripts/arduino-cli.sh"

seeed_index="https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json"
"$cli" core update-index --additional-urls "$seeed_index"
"$cli" core install \
  --additional-urls "$seeed_index" \
  "Seeeduino:mbed@2.9.3"

libraries=(
  "Adafruit BME280 Library@2.3.0"
  "Adafruit BusIO@1.17.4"
  "Adafruit Unified Sensor@1.1.15"
  "Adafruit VEML7700 Library@2.1.6"
  "Sensirion I2C SCD4x@1.1.0"
  "Sensirion Core@0.7.3"
  "Adafruit GFX Library@1.12.6"
  "GxEPD2@1.6.9"
  "ArduinoBLE@2.1.0"
)

for library in "${libraries[@]}"; do
  "$cli" lib install "$library"
done
