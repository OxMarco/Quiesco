// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

class Scheduler {
 public:
  static constexpr uint64_t kMillisecondsPerSecond = 1000;

  void begin(uint64_t nowMs, uint32_t intervalSeconds);
  bool claimDue(uint64_t nowMs);
  void changeInterval(uint64_t nowMs, uint32_t intervalSeconds);

  uint64_t nextDeadlineMs() const;
  uint32_t intervalSeconds() const;

 private:
  uint64_t intervalMs_ = 0;
  uint64_t nextDeadlineMs_ = 0;
  bool started_ = false;
};
