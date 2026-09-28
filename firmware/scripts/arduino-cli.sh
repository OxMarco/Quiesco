#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail

bundled_cli="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"

if command -v arduino-cli >/dev/null 2>&1; then
  cli="$(command -v arduino-cli)"
elif [[ -x "$bundled_cli" ]]; then
  cli="$bundled_cli"
else
  echo "arduino-cli was not found in PATH or the Arduino IDE bundle." >&2
  exit 1
fi

exec "$cli" "$@"
