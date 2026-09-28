// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

class HardwareWatchdog {
 public:
  static constexpr uint32_t kTimeoutMs = 30000;

  void begin();
  void kick();

 private:
  bool active_ = false;
};
