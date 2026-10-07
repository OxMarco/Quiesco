// SPDX-License-Identifier: GPL-3.0-only
#include "ComfortEvaluation.h"

namespace {

constexpr float kNoLowEdge = -1.0e9f;
constexpr float kNoHighEdge = 1.0e9f;

constexpr MetricBand kBands[kUiMetricCount] = {
    {kNoLowEdge, kNoLowEdge, 800.0f, 1200.0f},  // CO2 ppm
    {18.0f, 20.0f, 26.0f, 28.0f},               // temperature C
    {25.0f, 30.0f, 60.0f, 70.0f},               // humidity %
    {kNoLowEdge, kNoLowEdge, 55.0f, 70.0f},     // noise dB
    {kNoLowEdge, kNoLowEdge, kNoHighEdge, kNoHighEdge},  // light: never judged
};

// By day noise is judged for hearing, not sleep: the EPA's 70 dB(A) 24-hour
// average and NIOSH's 85 dB(A) 8-hour limit (the app's DAY_NOISE).
constexpr MetricBand kDayNoiseBand = {kNoLowEdge, kNoLowEdge, 70.0f, 85.0f};
constexpr MetricBand kUnjudged = {kNoLowEdge, kNoLowEdge, kNoHighEdge,
                                  kNoHighEdge};

}  // namespace

bool isJudged(UiMetric metric, JudgeMode mode) {
  switch (metric) {
    case UiMetric::kCo2:
    case UiMetric::kNoise:
      return true;
    case UiMetric::kTemperature:
    case UiMetric::kHumidity:
      return mode == JudgeMode::kSleep;
    case UiMetric::kLight:
      return false;
  }
  return false;
}

const MetricBand& metricBand(UiMetric metric, JudgeMode mode) {
  if (!isJudged(metric, mode)) {
    return kUnjudged;
  }
  if (metric == UiMetric::kNoise && mode == JudgeMode::kDay) {
    return kDayNoiseBand;
  }
  return kBands[static_cast<uint8_t>(metric)];
}

Severity bandSeverity(UiMetric metric, float value, JudgeMode mode) {
  const MetricBand& band = metricBand(metric, mode);
  if (value < band.warnLo || value > band.warnHi) {
    return Severity::kBad;
  }
  if (value < band.okLo || value > band.okHi) {
    return Severity::kWarn;
  }
  return Severity::kOk;
}
