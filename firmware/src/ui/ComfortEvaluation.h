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

// Which verdicts apply (UI.md §3, the app's judgeMode). kSleep judges every
// metric on the sleep bands; it is also the all-day behaviour when the panel
// does not follow the sleep window. kDay judges only CO2 (same band) and
// noise (hearing band); temperature and humidity are shown but not judged.
enum class JudgeMode : uint8_t { kSleep, kDay };

// Comfortable inside [okLo, okHi], warn inside [warnLo, warnHi], bad outside.
// One-sided bands use a large negative sentinel for the unused low edges.
struct MetricBand {
  float warnLo, okLo, okHi, warnHi;
};

// False for a metric that gets no verdict in this mode: it is always kOk and
// has no band to draw. Light is never judged.
bool isJudged(UiMetric metric, JudgeMode mode);
const MetricBand& metricBand(UiMetric metric,
                             JudgeMode mode = JudgeMode::kSleep);
Severity bandSeverity(UiMetric metric, float value,
                      JudgeMode mode = JudgeMode::kSleep);
