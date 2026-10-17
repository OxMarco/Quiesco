// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <math.h>
#include <stdint.h>

// Counts to lux for the VEML7700, free of Arduino types for the host tests.
namespace Veml7700Lux {

// Current datasheet (rev 1.8): 0.0042 lx/count at gain 2 and 800 ms,
// scaling inversely with gain and integration time.
constexpr float kFinestResolutionLux = 0.0042f;
// The sensor's rated range. Vishay's correction polynomial is a fit to that
// range; past it the quartic term runs away (raw 30 klx came out as 337 klx),
// and such readings were dropped as invalid, so direct sun logged a fault.
constexpr float kMaximumLux = 120000.0f;
// At or above this the 16-bit counter is saturated: the light changed after
// the range was chosen, and the counts are a floor, not a reading.
constexpr uint16_t kSaturatedCounts = 65000;

inline float resolutionLux(float gain, float integrationMs) {
  return kFinestResolutionLux * (800.0f / integrationMs) * (2.0f / gain);
}

// Vishay's non-linearity correction for the low gains (application note
// 84323), held to the rated range.
inline float correctedLux(float lux) {
  const float corrected =
      (((6.0135e-13f * lux - 9.3924e-9f) * lux + 8.1488e-5f) * lux + 1.0023f) *
      lux;
  return corrected > kMaximumLux ? kMaximumLux : corrected;
}

inline bool saturated(uint16_t counts) { return counts >= kSaturatedCounts; }

inline bool validLux(float value) {
  return isfinite(value) && value >= 0.0f && value <= kMaximumLux;
}

}  // namespace Veml7700Lux
