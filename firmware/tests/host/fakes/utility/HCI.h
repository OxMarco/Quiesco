// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

// Host fake of ArduinoBLE's HCI singleton: only what BleConfig uses.
class HCIClass {
 public:
  int leRand(uint8_t rand[]);
};

extern HCIClass HCI;
