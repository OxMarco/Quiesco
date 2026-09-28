// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

// Stateless comfort policy shared by every screen (bands per UI.md). Pure
// C++ with no Arduino dependency so it stays host-testable.

enum class Severity : uint8_t { kOk, kWarn, kBad };

// Order is the tie-break priority when several metrics are equally bad.
enum class UiMetric : uint8_t {
  kCo2 = 0,
  kTemperature,
  kHumidity,
  kNoise,
  kLight,  // informational only: never drives the face
};

constexpr uint8_t kUiMetricCount = 5;

// Comfortable inside [okLo, okHi], warn inside [warnLo, warnHi], bad outside.
// One-sided bands use a large negative sentinel for the unused low edges.
struct MetricBand {
  float warnLo, okLo, okHi, warnHi;
};

const MetricBand& metricBand(UiMetric metric);
Severity bandSeverity(UiMetric metric, float value);
