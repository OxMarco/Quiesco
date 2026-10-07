// SPDX-License-Identifier: GPL-3.0-only
#include "BootloaderEntry.h"

#include <Arduino.h>

namespace {

// DFU_MAGIC_OTA_RESET and DFU_MAGIC_UF2_RESET in the bootloader's main.c.
constexpr uint32_t kOtaResetMagic = 0xA8;
constexpr uint32_t kUf2ResetMagic = 0x57;

void resetInto(uint32_t magic) {
  // No SoftDevice runs under the mbed core, so POWER is ours to write.
  NRF_POWER->GPREGRET = magic;
  NVIC_SystemReset();
}

}  // namespace

void BootloaderEntry::enterOtaUpdate() { resetInto(kOtaResetMagic); }

void BootloaderEntry::enterUsbUpdate() { resetInto(kUf2ResetMagic); }
