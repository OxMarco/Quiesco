// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Hands the MCU to the XIAO's Adafruit nRF52 bootloader in BLE OTA mode
// (GPREGRET 0xA8, then a soft reset). The bootloader starts the SoftDevice,
// advertises as "AdaDFU" and takes a Nordic legacy DFU package; it feeds the
// watchdog meanwhile. Config, keys and the log are in external flash, which
// it never touches. Does not return.
namespace BootloaderEntry {

void enterOtaUpdate();

// Same bootloader in UF2 mode: on USB it shows the XIAO-BOOT drive, and a
// .uf2 copied onto it flashes the firmware. Without USB enumerating within
// 3 s it starts the firmware again. Does not return.
void enterUsbUpdate();

}  // namespace BootloaderEntry
