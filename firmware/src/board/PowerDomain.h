// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

class PowerDomain {
 public:
  static constexpr uint32_t kSettleMs = 1000;
  // For cycles that touch only the SPI bus (e-ink, flash): the W25Q64 takes
  // writes ~10 ms after power-up (tPUW) and the panel's own reset BUSY wait
  // covers the rest. Measurements keep kSettleMs for the sensors.
  static constexpr uint32_t kBusSettleMs = 50;

  void beginOff();
  void enable(uint64_t nowMs, uint32_t settleMs = kSettleMs);
  bool ready(uint64_t nowMs) const;
  void disable();
  bool enabled() const;

 private:
  void parkPoweredPins();
  void parkRailOffPins();

  bool enabled_ = false;
  uint64_t readyAtMs_ = 0;
};
