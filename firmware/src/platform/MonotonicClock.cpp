// SPDX-License-Identifier: GPL-3.0-only
#include "MonotonicClock.h"

#include <Arduino.h>

uint64_t MonotonicClock::nowMs() {
  const uint32_t low = millis();
  if (initialized_ && low < previousLow_) {
    high_ += uint64_t{1} << 32;
  }
  initialized_ = true;
  previousLow_ = low;
  return high_ | low;
}
