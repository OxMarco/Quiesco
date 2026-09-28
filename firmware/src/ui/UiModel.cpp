// SPDX-License-Identifier: GPL-3.0-only
#include "UiModel.h"

#include <string.h>

#include "../model/Config.h"
#include "../model/Reading.h"

namespace {

constexpr uint32_t kMetricValidFlag[kUiMetricCount] = {
    VALID_CO2, VALID_TEMPERATURE, VALID_HUMIDITY, VALID_NOISE, VALID_LIGHT,
};

float metricValue(const Reading& reading, UiMetric metric) {
  switch (metric) {
    case UiMetric::kCo2:
      return reading.co2Ppm;
    case UiMetric::kTemperature:
      return reading.temperatureC;
    case UiMetric::kHumidity:
      return reading.humidityPct;
    case UiMetric::kNoise:
      return reading.noiseDb;
    case UiMetric::kLight:
      return reading.lux;
  }
  return 0.0f;
}

int32_t roundToInt(float value) {
  return static_cast<int32_t>(value + (value >= 0.0f ? 0.5f : -0.5f));
}

// Resting-voltage breakpoints for a 1S LiPo; linear between points.
struct VoltsPercent {
  float volts;
  uint8_t percent;
};
constexpr VoltsPercent kBatteryCurve[] = {
    {3.30f, 0}, {3.50f, 5}, {3.70f, 25}, {3.80f, 50}, {3.95f, 75}, {4.20f, 100},
};

bool haveSameMetrics(const UiModel& a, const UiModel& b) {
  for (uint8_t i = 0; i < kUiMetricCount; ++i) {
    if (a.values[i] != b.values[i] || a.severities[i] != b.severities[i]) {
      return false;
    }
  }
  return true;
}

}  // namespace

uint8_t batteryPercentFromVolts(float volts) {
  constexpr int count = sizeof(kBatteryCurve) / sizeof(kBatteryCurve[0]);
  if (volts <= kBatteryCurve[0].volts) {
    return 0;
  }
  for (int i = 1; i < count; ++i) {
    if (volts <= kBatteryCurve[i].volts) {
      const VoltsPercent& lo = kBatteryCurve[i - 1];
      const VoltsPercent& hi = kBatteryCurve[i];
      const float t = (volts - lo.volts) / (hi.volts - lo.volts);
      return static_cast<uint8_t>(lo.percent +
                                  t * (hi.percent - lo.percent) + 0.5f);
    }
  }
  return 100;
}

UiModel buildUiModel(const Reading& reading, uint8_t configuredScreen,
                     uint8_t temperatureUnit, bool charging) {
  UiModel model;
  model.fahrenheit = temperatureUnit == TEMPERATURE_UNIT_FAHRENHEIT;

  // Judged on the rounded °C value whatever the unit, so the verdict never
  // depends on how the digits are written.
  int32_t judged[kUiMetricCount] = {};
  for (uint8_t i = 0; i < kUiMetricCount; ++i) {
    const UiMetric metric = static_cast<UiMetric>(i);
    if (!(reading.valid & kMetricValidFlag[i])) {
      continue;
    }
    model.validMask |= 1u << i;
    const float value = metricValue(reading, metric);
    judged[i] = roundToInt(value);
    model.severities[i] = bandSeverity(metric, static_cast<float>(judged[i]));
    model.values[i] = metric == UiMetric::kTemperature && model.fahrenheit
                          ? roundToInt(value * 9.0f / 5.0f + 32.0f)
                          : judged[i];
  }

  // Worst valid metric wins the face; enum order is the tie-break priority.
  for (uint8_t i = 0; i < kUiMetricCount; ++i) {
    const UiMetric metric = static_cast<UiMetric>(i);
    if (metric == UiMetric::kLight || !(model.validMask & (1u << i))) {
      continue;
    }
    if (model.severities[i] > model.faceSeverity) {
      model.faceSeverity = model.severities[i];
      model.worstMetric = metric;
      model.worstAbove = judged[i] > metricBand(metric).okHi;
    }
  }

  const bool batteryValid = reading.valid & VALID_BATTERY;
  if (batteryValid) {
    const uint8_t percent = batteryPercentFromVolts(reading.batteryV);
    model.batteryPercent = static_cast<uint8_t>(((percent + 2) / 5) * 5);
  }
  model.charging = charging;

  const bool batteryLow = batteryValid && reading.batteryV < kLowBatteryVolts;
  if (charging || batteryLow) {
    model.screen = ScreenId::kBattery;
  } else if (configuredScreen == DISPLAY_SCREEN_LEDGER) {
    model.screen = ScreenId::kLedger;
  } else if (configuredScreen == DISPLAY_SCREEN_BENTO) {
    model.screen = ScreenId::kBento;
  } else {
    model.screen = ScreenId::kFace;
  }
  constexpr uint8_t kComfortMetricMask =
      (1u << static_cast<uint8_t>(UiMetric::kCo2)) |
      (1u << static_cast<uint8_t>(UiMetric::kTemperature)) |
      (1u << static_cast<uint8_t>(UiMetric::kHumidity)) |
      (1u << static_cast<uint8_t>(UiMetric::kNoise));
  if (model.screen == ScreenId::kFace &&
      (model.validMask & kComfortMetricMask) == 0) {
    model.screen = ScreenId::kUnavailable;
  }
  return model;
}

bool hasSameRenderedContent(const UiModel& a, const UiModel& b) {
  if (a.screen != b.screen || a.fahrenheit != b.fahrenheit) {
    return false;
  }
  switch (a.screen) {
    case ScreenId::kFace: {
      if (a.faceSeverity != b.faceSeverity) {
        return false;
      }
      if (a.faceSeverity == Severity::kOk) {
        return true;
      }
      const uint8_t ai = static_cast<uint8_t>(a.worstMetric);
      const uint8_t bi = static_cast<uint8_t>(b.worstMetric);
      return a.worstMetric == b.worstMetric && a.worstAbove == b.worstAbove &&
             a.values[ai] == b.values[bi];
    }
    case ScreenId::kLedger:
    case ScreenId::kBento:
      if (a.validMask != b.validMask) {
        return false;
      }
      return haveSameMetrics(a, b);
    case ScreenId::kBattery:
      return a.batteryPercent == b.batteryPercent &&
             a.charging == b.charging;
    case ScreenId::kUnavailable:
      return true;
    case ScreenId::kPairing:
      return a.pairingCode == b.pairingCode;
  }
  return false;
}

UiModel buildPairingModel(uint32_t code) {
  UiModel model;
  model.screen = ScreenId::kPairing;
  model.pairingCode = code;
  return model;
}
