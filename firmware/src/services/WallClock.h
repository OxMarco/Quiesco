// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

// Anchors the BLE-supplied Unix epoch to the monotonic clock. Purely RAM:
// the epoch is intentionally lost on reset (no RTC) and the app re-syncs on
// connect, so records written across a reboot are never mis-stamped by a
// stale anchor.
class WallClock {
 public:
  void sync(uint64_t epochMs, uint64_t monotonicNowMs) {
    epochAtSyncMs_ = epochMs;
    monotonicAtSyncMs_ = monotonicNowMs;
    synced_ = epochMs != 0;
  }

  bool synced() const { return synced_; }

  // Wall-clock for any monotonic instant, before or after the sync point;
  // 0 when never synced.
  uint64_t epochMsAt(uint64_t monotonicMs) const {
    if (!synced_) {
      return 0;
    }
    if (monotonicMs >= monotonicAtSyncMs_) {
      return epochAtSyncMs_ + (monotonicMs - monotonicAtSyncMs_);
    }
    const uint64_t before = monotonicAtSyncMs_ - monotonicMs;
    return before < epochAtSyncMs_ ? epochAtSyncMs_ - before : 0;
  }

 private:
  uint64_t epochAtSyncMs_ = 0;
  uint64_t monotonicAtSyncMs_ = 0;
  bool synced_ = false;
};
