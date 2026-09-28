// SPDX-License-Identifier: GPL-3.0-only
#include "ResetReason.h"

#include <Arduino.h>

namespace ResetReason {

uint32_t take() {
  const uint32_t bits = NRF_POWER->RESETREAS;
  NRF_POWER->RESETREAS = bits;  // write-one-to-clear
  return bits;
}

const char* describe(uint32_t bits) {
  if (bits & kWatchdog) {
    return "watchdog";
  }
  if (bits & kLockup) {
    return "lockup";
  }
  if (bits & kSoftReset) {
    return "soft";
  }
  if (bits & kResetPin) {
    return "pin";
  }
  if (bits & (kSystemOffWake | kVbusWake)) {
    return "wake";
  }
  return bits == 0 ? "power-on" : "other";
}

}  // namespace ResetReason
