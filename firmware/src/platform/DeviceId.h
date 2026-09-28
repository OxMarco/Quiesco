// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

// The nRF52840's factory-programmed 64-bit device ID (FICR DEVICEID), stable
// for the life of the chip. Source of the unit serial (UnitIdentity).
namespace DeviceId {

uint64_t read();

}  // namespace DeviceId
