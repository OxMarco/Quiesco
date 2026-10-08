// SPDX-License-Identifier: GPL-3.0-only
#include "UnitIdentity.h"

#include <stdio.h>
#include <string.h>

namespace UnitIdentity {

void formatSerial(uint64_t deviceId, char out[kSerialChars + 1]) {
  static const char kHex[] = "0123456789ABCDEF";
  for (uint32_t i = 0; i < kSerialChars; i++) {
    const uint32_t shift = (kSerialChars - 1 - i) * 4;
    out[i] = kHex[(deviceId >> shift) & 0xF];
  }
  out[kSerialChars] = '\0';
}

Config defaultConfigForUnit(uint64_t deviceId) {
  Config config = defaultConfig();
  char base[sizeof config.deviceName];
  memcpy(base, config.deviceName, sizeof base);
  char serial[kSerialChars + 1];
  formatSerial(deviceId, serial);
  // The base is capped so the serial suffix always fits.
  const int baseChars =
      static_cast<int>(sizeof config.deviceName - 2 - kNameSuffixChars);
  snprintf(config.deviceName, sizeof config.deviceName, "%.*s %s", baseChars,
           base, serial + kSerialChars - kNameSuffixChars);
  return config;
}

}  // namespace UnitIdentity
