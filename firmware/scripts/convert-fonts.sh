#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Regenerates the vendored bitmap-font headers the renderer links against:
#   src/ui/fonts/Fredoka{Regular,SemiBold}{7,9}pt.h, FredokaSemiBold{12,24}pt.h
#   (character ranges per cut are listed at the bottom)
#
# Pipeline: fonttools (venv, installed on first run) instances the Fredoka
# variable font into static weights, then Adafruit fontconvert (compiled from
# the installed GFX library) emits the GFXfont headers. Sources are cached in
# .build/fonts; ranges are decimal because fontconvert parses them with atoi().
#
# Only the cuts EpaperDisplay::setFont actually selects are generated. Adding a
# weight/size here means adding a matching Font enum case in EpaperDisplay.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$root/.build/fonts"
out="$root/src/ui/fonts"
mkdir -p "$work"

FREDOKA_URL="https://github.com/google/fonts/raw/main/ofl/fredoka/Fredoka%5Bwdth%2Cwght%5D.ttf"

fetch() {
  local url="$1" out="$2"
  if [ ! -s "$out" ]; then
    echo "fetching $(basename "$out")"
    curl -sSfL -o "$out" "$url"
  fi
}
fetch "$FREDOKA_URL" "$work/fredoka-vf.ttf"
# The OFL requires its text to travel with the font; the bitmaps are shipped.
fetch "https://github.com/google/fonts/raw/main/ofl/fredoka/OFL.txt" \
  "$work/OFL.txt"
cp "$work/OFL.txt" "$out/OFL.txt"

# fontconvert from the installed (pinned) Adafruit GFX library.
if [ ! -x "$work/fontconvert" ]; then
  src="$HOME/Documents/Arduino/libraries/Adafruit_GFX_Library/fontconvert/fontconvert.c"
  [ -f "$src" ] || { echo "Adafruit GFX library (fontconvert) not installed" >&2; exit 1; }
  freetype="$(brew --prefix freetype 2>/dev/null || echo /usr)"
  cc -o "$work/fontconvert" "$src" \
    -I"$freetype/include/freetype2" -L"$freetype/lib" -lfreetype
fi

# fonttools in a local venv.
if [ ! -x "$work/venv/bin/python" ]; then
  python3 -m venv "$work/venv"
  "$work/venv/bin/pip" -q install fonttools
fi
python="$work/venv/bin/python"

# Fredoka: static weight instances from the variable font.
for spec in 400:Regular 600:SemiBold; do
  wght="${spec%%:*}"; name="${spec##*:}"
  if [ ! -s "$work/Fredoka-$name.ttf" ]; then
    "$python" -m fontTools.varLib.instancer "$work/fredoka-vf.ttf" \
      "wght=$wght" "wdth=100" -o "$work/Fredoka-$name.ttf" >/dev/null 2>&1
  fi
done

convert() {  # ttf size first last out
  "$work/fontconvert" "$1" "$2" "$3" "$4" > "$5"
  echo "generated $(basename "$5")"
}

# name:size:first:last. The 24pt cut is the bento hero only, which prints digits
# and the "--" placeholder, so it carries 45-57 ('-' './' 0-9) instead of the
# full 32-181 range -- a full cut at that size would cost ~16 KB of flash.
# The 7pt pair is the ledger's, which is ASCII-only, so it stops at 126.
for spec in Regular:7:32:126 SemiBold:7:32:126 Regular:9:32:181 SemiBold:9:32:181 SemiBold:12:32:181 SemiBold:24:45:57; do
  IFS=: read -r name size first last <<< "$spec"
  convert "$work/Fredoka-$name.ttf" "$size" "$first" "$last" \
    "$out/Fredoka${name}${size}pt.h"
done
