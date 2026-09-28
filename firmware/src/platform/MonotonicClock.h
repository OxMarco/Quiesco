// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

class MonotonicClock {
 public:
  uint64_t nowMs();

 private:
  uint32_t previousLow_ = 0;
  uint64_t high_ = 0;
  bool initialized_ = false;
};
