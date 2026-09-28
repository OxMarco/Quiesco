// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

#include "../model/Config.h"

// Fixed product identity and the per-unit serial. Pure C++ (host-tested); the
// 64-bit device ID itself comes from platform/DeviceId.
namespace UnitIdentity {

// Device Information Service strings. Product owns these values.
constexpr char kManufacturer[] = "Quiesco";
constexpr char kModel[] = "Quiesco v1";
constexpr char kHardwareRevision[] = "1";

constexpr uint32_t kSerialChars = 16;  // 64-bit ID as uppercase hex
constexpr uint32_t kNameSuffixChars = 4;

// Uppercase hex, most significant nibble first, NUL-terminated.
void formatSerial(uint64_t deviceId, char out[kSerialChars + 1]);

// defaultConfig() named "Quiesco XXXX" after the last four serial digits, so
// two fresh units on one desk are told apart before anyone renames them.
Config defaultConfigForUnit(uint64_t deviceId);

}  // namespace UnitIdentity
