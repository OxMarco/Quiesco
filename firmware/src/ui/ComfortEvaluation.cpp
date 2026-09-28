// SPDX-License-Identifier: GPL-3.0-only
#include "ComfortEvaluation.h"

namespace {

constexpr float kNoLowEdge = -1.0e9f;

constexpr MetricBand kBands[kUiMetricCount] = {
    {kNoLowEdge, kNoLowEdge, 800.0f, 1200.0f},  // CO2 ppm
    {18.0f, 20.0f, 26.0f, 28.0f},               // temperature C
    {25.0f, 30.0f, 60.0f, 70.0f},               // humidity %
    {kNoLowEdge, kNoLowEdge, 55.0f, 70.0f},     // noise dB
    {kNoLowEdge, kNoLowEdge, 1.0e9f, 1.0e9f},   // light: always comfortable
};

}  // namespace

const MetricBand& metricBand(UiMetric metric) {
  return kBands[static_cast<uint8_t>(metric)];
}

Severity bandSeverity(UiMetric metric, float value) {
  const MetricBand& band = metricBand(metric);
  if (value < band.warnLo || value > band.warnHi) {
    return Severity::kBad;
  }
  if (value < band.okLo || value > band.okHi) {
    return Severity::kWarn;
  }
  return Severity::kOk;
}
