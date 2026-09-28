// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

struct Reading;

class Bme280Sensor {
 public:
  static constexpr uint32_t kMeasurementTimeoutMs = 100;

  bool begin();
  void start(uint64_t nowMs);
  void poll(uint64_t nowMs);
  bool ready() const;
  void collect(Reading& reading);
  void end();

  bool present() const;
  bool timedOut() const;

 private:
  enum class State : uint8_t {
    kUnavailable,
    kIdle,
    kMeasuring,
    kComplete,
    kTimedOut,
  };

  State state_ = State::kUnavailable;
  uint64_t deadlineMs_ = 0;
  uint8_t address_ = 0;
};
