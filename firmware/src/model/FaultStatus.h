// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

struct DeviceFault {
  bool present = false;
  bool timedOut = false;
  uint16_t consecutiveFailures = 0;
};

struct FaultStatus {
  DeviceFault bme280;
  DeviceFault veml7700;
  DeviceFault scd41;
  DeviceFault battery;
  DeviceFault microphone;
  DeviceFault display;
  DeviceFault flash;
  DeviceFault ble;
};

inline void recordDeviceResult(DeviceFault& fault, bool present, bool timedOut,
                               bool succeeded) {
  fault.present = present;
  fault.timedOut = timedOut;
  if (succeeded) {
    fault.consecutiveFailures = 0;
  } else if (fault.consecutiveFailures < UINT16_MAX) {
    ++fault.consecutiveFailures;
  }
}
