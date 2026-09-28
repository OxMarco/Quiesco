// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

struct Reading;

// VEML7700 ambient light, auto-ranged per Vishay's application note
// "Designing the VEML7700 Into an Application" (84323, fig. 24), without
// blocking: a coarse read at gain 1/8 and 100 ms; below 100 counts (~54 lx)
// one sensitive re-read at gain 2 and 400 ms (0.0084 lx/count, linear);
// above 10 000 counts a 25 ms re-read with the non-linearity correction.
// Counts become lux with the current datasheet's 0.0042 lx/count at gain 2 /
// 800 ms (rev 1.8, 2024), not the 0.0036 older revisions and the Adafruit
// library use. The case window's transmission is not included: absolute lux
// needs a calibration against a lux meter.
class Veml7700Sensor {
 public:
  // Vishay: wait at least one integration time; Adafruit measured that 2x is
  // needed in practice, so each read waits 2.5x.
  static constexpr uint32_t kCoarseWaitMs = 250;
  static constexpr uint32_t kFineWaitMs = 1000;
  static constexpr uint32_t kBrightWaitMs = 70;
  // A failed I2C transfer is retried after kRetryMs. The timeout leaves room
  // for retries and still ends well inside the SCD41's ~10 s shot, so the
  // rail stays up no longer.
  static constexpr uint32_t kRetryMs = 50;
  static constexpr uint32_t kMeasurementTimeoutMs = 5000;

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
    kCoarse,
    kFine,    // low light: gain 2, 400 ms
    kBright,  // bright light: gain 1/8, 25 ms
    kComplete,
    kTimedOut,
  };

  State state_ = State::kUnavailable;
  uint64_t readyAtMs_ = 0;
  uint64_t timeoutAtMs_ = 0;
  float lux_ = 0.0f;
};
