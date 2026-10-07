#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Wraps the build's application image in UF2 for the XIAO's bootloader.

  scripts/make-uf2.py firmware.ino.bin firmware.uf2

Copying the result onto the XIAO-BOOT drive flashes it (AGENT.md at the
repository root). The image starts after SoftDevice S140 7.3.0, where the core's
linker script puts it; the family ID is the Adafruit nRF52840 one the
bootloader checks.
"""

import struct
import sys

APP_START = 0x27000         # variants/SEEED_XIAO_NRF52840_PLUS/linker_script.ld
APP_END = 0xED000
FAMILY_NRF52840 = 0xADA52840
MAGIC_START0, MAGIC_START1, MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
FLAG_FAMILY_ID = 0x00002000
PAYLOAD = 256               # bytes per block, as the bootloader writes them


def to_uf2(image, base=APP_START):
    if base + len(image) > APP_END:
        sys.exit(f"image of {len(image)} bytes overruns the application region")
    count = (len(image) + PAYLOAD - 1) // PAYLOAD
    blocks = []
    for index in range(count):
        chunk = image[index * PAYLOAD:(index + 1) * PAYLOAD]
        header = struct.pack("<8I", MAGIC_START0, MAGIC_START1, FLAG_FAMILY_ID,
                             base + index * PAYLOAD, PAYLOAD, index, count,
                             FAMILY_NRF52840)
        data = chunk.ljust(476, b"\x00")
        blocks.append(header + data + struct.pack("<I", MAGIC_END))
    return b"".join(blocks)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as handle:
        image = handle.read()
    with open(sys.argv[2], "wb") as handle:
        handle.write(to_uf2(image))


if __name__ == "__main__":
    main()
