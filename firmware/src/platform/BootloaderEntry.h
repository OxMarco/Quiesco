// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Hands the MCU to the XIAO's Adafruit nRF52 bootloader in UF2 mode
// (GPREGRET 0x57, then a soft reset): on USB it shows the XIAO-BOOT drive, and
// a .uf2 copied onto it flashes the firmware. Without USB enumerating within
// 3 s it starts the firmware again. Config, keys and the log are in external
// flash, which it never touches. Does not return.
namespace BootloaderEntry {

void enterUsbUpdate();

}  // namespace BootloaderEntry
