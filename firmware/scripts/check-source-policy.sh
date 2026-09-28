#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_root="$root/src"
failed=0

# grep, not rg: a missing rg made every check below exit non-zero, which this
# script read as "no violations" and reported as a pass. Third-party sources
# are excluded; they are vendored, not held to the project's pin policy.
scan() {  # pattern [extra grep args...]
  local pattern="$1"
  shift
  grep -rnE --include='*.h' --include='*.cpp' \
    --exclude-dir=third_party "$@" -- "$pattern" "$source_root"
}

if scan '\b(D[0-9]+|LED[RGB])\b' --exclude=BoardPins.h; then
  echo "Board pin macros are forbidden outside BoardPins.h."
  failed=1
fi

if scan '\b(pinMode|digitalWrite|digitalRead|analogRead)[[:space:]]*\([[:space:]]*[0-9]+' \
  --exclude=BoardPins.h; then
  echo "Raw Arduino pin numbers are forbidden outside BoardPins.h."
  failed=1
fi

if scan '\banalogRead[[:space:]]*\('; then
  echo "analogRead() is forbidden in production; battery sampling must use SAADC."
  failed=1
fi

if scan '#include[[:space:]]*[<"](Adafruit_|Sensirion|GxEPD2|ArduinoBLE)' \
  --exclude-dir=drivers; then
  echo "Third-party hardware and protocol headers belong inside adapters."
  failed=1
fi

# The firmware is GPL-3.0-only (LICENSE). Vendored code and generated fonts
# keep their own licences and are exempt.
missing_spdx="$(grep -rL --include='*.h' --include='*.cpp' --include='*.ino' \
  --include='*.sh' --include='*.py' --exclude-dir=third_party --exclude-dir=fonts \
  'SPDX-License-Identifier: GPL-3.0-only' \
  "$source_root" "$root/tests" "$root/smoke" "$root/scripts" \
  "$root/firmware.ino" || true)"
if [ -n "$missing_spdx" ]; then
  echo "Missing 'SPDX-License-Identifier: GPL-3.0-only' in:"
  echo "$missing_spdx"
  failed=1
fi

if [ "$failed" -eq 0 ]; then
  echo "Source policy checks passed."
fi
exit "$failed"
