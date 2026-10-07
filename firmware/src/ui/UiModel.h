// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

#include "ComfortEvaluation.h"

struct Reading;

// Everything the renderer needs, pre-rounded so two models compare equal
// whenever the drawn pixels would be identical (skip-if-unchanged policy).

enum class ScreenId : uint8_t {
  kFace,
  kLedger,
  kBento,
  kBattery,
  kUnavailable,
  kPairing,  // local-only setup key; overrides all
};

struct UiModel {
  ScreenId screen = ScreenId::kFace;
  uint8_t validMask = 0;                     // bit index = UiMetric
  int32_t values[kUiMetricCount] = {};       // rounded display values
  bool fahrenheit = false;  // values[kTemperature] is °F; bands stay °C
  // Which verdicts apply (SleepSchedule); kDay leaves temperature and
  // humidity unjudged and judges noise on the hearing band.
  JudgeMode mode = JudgeMode::kSleep;
  Severity severities[kUiMetricCount] = {};  // judged on the rounded values
  Severity faceSeverity = Severity::kOk;
  UiMetric worstMetric = UiMetric::kCo2;
  bool worstAbove = false;
  uint8_t batteryPercent = 0;  // rounded to 5% steps to bound refreshes
  bool charging = false;
  uint32_t pairingCode = 0;  // six digits
};

// Compares only what the model's screen actually draws, so a metric that
// changed off-screen does not cost a panel refresh.
bool hasSameRenderedContent(const UiModel& a, const UiModel& b);

// Battery presentation policy. The low threshold is resting voltage; below it
// the device shows the battery screen instead of the configured one.
constexpr float kLowBatteryVolts = 3.5f;
uint8_t batteryPercentFromVolts(float volts);

// configuredScreen is Config::displayScreen and temperatureUnit
// Config::temperatureUnit (both validated); charging comes from the charger
// status pin, already debounced; mode from SleepSchedule::panelJudgeMode.
UiModel buildUiModel(const Reading& reading, uint8_t configuredScreen,
                     uint8_t temperatureUnit, bool charging,
                     JudgeMode mode = JudgeMode::kSleep);

// Local-only setup code. The phone must prove it before it is given a key.
UiModel buildPairingModel(uint32_t code);
