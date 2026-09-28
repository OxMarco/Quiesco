// SPDX-License-Identifier: GPL-3.0-only
#include "Scheduler.h"

void Scheduler::begin(uint64_t nowMs, uint32_t intervalSeconds) {
  intervalMs_ = uint64_t{intervalSeconds} * kMillisecondsPerSecond;
  nextDeadlineMs_ = nowMs;
  started_ = true;
}

bool Scheduler::claimDue(uint64_t nowMs) {
  if (!started_ || nowMs < nextDeadlineMs_) {
    return false;
  }

  const uint64_t elapsed = nowMs - nextDeadlineMs_;
  const uint64_t intervalsElapsed = elapsed / intervalMs_;
  nextDeadlineMs_ += (intervalsElapsed + 1) * intervalMs_;
  return true;
}

void Scheduler::changeInterval(uint64_t nowMs, uint32_t intervalSeconds) {
  intervalMs_ = uint64_t{intervalSeconds} * kMillisecondsPerSecond;
  nextDeadlineMs_ = nowMs + intervalMs_;
  started_ = true;
}

uint64_t Scheduler::nextDeadlineMs() const {
  return nextDeadlineMs_;
}

uint32_t Scheduler::intervalSeconds() const {
  return static_cast<uint32_t>(intervalMs_ / kMillisecondsPerSecond);
}
