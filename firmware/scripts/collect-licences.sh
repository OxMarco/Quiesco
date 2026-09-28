#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Gathers the licence text of everything linked into the firmware into
# .build/licences/, next to THIRD_PARTY_NOTICES.md, for a release package.
# Fails if an installed library or the core differs from dependencies.lock, so
# the texts always match what was built.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$root/.build/licences"
libraries="${ARDUINO_LIBRARIES:-$HOME/Documents/Arduino/libraries}"
core="${ARDUINO_DATA:-$HOME/Library/Arduino15}/packages/Seeeduino/hardware/mbed"
failed=0

rm -rf "$out"
mkdir -p "$out"

copy_licences() {  # source-dir destination-name
  local found=0
  for file in "$1"/LICENSE* "$1"/LICENCE* "$1"/license* "$1"/COPYING* "$1"/OFL.txt; do
    if [ -f "$file" ]; then
      mkdir -p "$out/$2"
      cp "$file" "$out/$2/"
      found=1
    fi
  done
  if [ "$found" -eq 0 ]; then
    echo "no licence file in $1" >&2
    failed=1
  fi
}

# "library <name with spaces> <version> <status>"; arduino-cli installs a
# library into a folder named after it with spaces as underscores.
while read -r kind rest; do
  case "$kind" in
    core)
      read -r _ version <<<"$rest"
      if [ ! -d "$core/$version" ]; then
        echo "core Seeeduino:mbed $version is not installed" >&2
        failed=1
      fi
      ;;
    library)
      status="${rest##* }"
      rest="${rest% *}"
      version="${rest##* }"
      name="${rest% *}"
      if [ "$status" = "vendored-production" ]; then
        continue
      fi
      dir="$libraries/${name// /_}"
      installed="$(sed -n 's/^version=//p' "$dir/library.properties" 2>/dev/null || true)"
      if [ "$installed" != "$version" ]; then
        echo "$name: locked $version, installed '${installed:-none}'" >&2
        failed=1
        continue
      fi
      copy_licences "$dir" "${name// /_}"
      ;;
  esac
done < <(grep -Ev '^\s*(#|$)' "$root/dependencies.lock")

copy_licences "$root/src/third_party/flashdb" FlashDB
cp "$root/src/third_party/flashdb/README.quiesco.md" "$out/FlashDB/CHANGES.md"
copy_licences "$root/src/ui/fonts" Fredoka
cp "$root/THIRD_PARTY_NOTICES.md" "$out/"

if [ "$failed" -ne 0 ]; then
  exit 1
fi
echo "Licences collected in $out"
