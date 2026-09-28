// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

// Why the MCU last reset: the nRF52840 POWER.RESETREAS bits, read and cleared
// once at boot (they accumulate otherwise). 0 means none of the recorded
// causes, i.e. a power-on or brownout reset.
namespace ResetReason {

enum Bits : uint32_t {
  kResetPin = 1u << 0,
  kWatchdog = 1u << 1,
  kSoftReset = 1u << 2,  // NVIC_SystemReset, including USB 1200-baud upload
  kLockup = 1u << 3,
  kSystemOffWake = 1u << 16,  // GPIO wake from System OFF
  kVbusWake = 1u << 20,
};

uint32_t take();

// Short name of the most significant cause, for the debug boot line.
const char* describe(uint32_t bits);

}  // namespace ResetReason
