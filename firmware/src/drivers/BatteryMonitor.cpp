// SPDX-License-Identifier: GPL-3.0-only
#include "BatteryMonitor.h"

#include <Arduino.h>

#include "../board/BoardPins.h"

namespace {

constexpr float kFullScaleVolts = 3.6f;  // 0.6 V internal ref / (1/6) gain
// VERIFY against a multimeter: divider assumed 1M / 510k (HARDWARE.md §4).
constexpr float kDividerRatio = (1000.0f + 510.0f) / 510.0f;

// SAADC events land within microseconds; this bounds a hardware fault, not a
// normal conversion.
bool waitEvent(volatile uint32_t& event) {
  for (uint32_t i = 0; i < 100000; ++i) {
    if (event) {
      return true;
    }
  }
  return false;
}

bool saadcSampleAin7(int16_t& out) {
  volatile int16_t result = 0;

  NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Enabled;
  NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_12bit;
  NRF_SAADC->OVERSAMPLE = SAADC_OVERSAMPLE_OVERSAMPLE_Bypass;
  NRF_SAADC->CH[0].PSELP = SAADC_CH_PSELP_PSELP_AnalogInput7;  // P0.31
  NRF_SAADC->CH[0].PSELN = SAADC_CH_PSELN_PSELN_NC;
  NRF_SAADC->CH[0].CONFIG =
      (SAADC_CH_CONFIG_RESP_Bypass << SAADC_CH_CONFIG_RESP_Pos) |
      (SAADC_CH_CONFIG_RESN_Bypass << SAADC_CH_CONFIG_RESN_Pos) |
      (SAADC_CH_CONFIG_GAIN_Gain1_6 << SAADC_CH_CONFIG_GAIN_Pos) |
      (SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) |
      (SAADC_CH_CONFIG_TACQ_10us << SAADC_CH_CONFIG_TACQ_Pos) |
      (SAADC_CH_CONFIG_MODE_SE << SAADC_CH_CONFIG_MODE_Pos) |
      (SAADC_CH_CONFIG_BURST_Disabled << SAADC_CH_CONFIG_BURST_Pos);
  NRF_SAADC->RESULT.PTR = reinterpret_cast<uint32_t>(&result);
  NRF_SAADC->RESULT.MAXCNT = 1;

  NRF_SAADC->EVENTS_STARTED = 0;
  NRF_SAADC->TASKS_START = 1;
  bool ok = waitEvent(NRF_SAADC->EVENTS_STARTED);
  if (ok) {
    NRF_SAADC->EVENTS_END = 0;
    NRF_SAADC->TASKS_SAMPLE = 1;
    ok = waitEvent(NRF_SAADC->EVENTS_END);
  }
  NRF_SAADC->EVENTS_STOPPED = 0;
  NRF_SAADC->TASKS_STOP = 1;
  waitEvent(NRF_SAADC->EVENTS_STOPPED);
  NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Disabled;

  out = result < 0 ? 0 : result;  // single-ended can read slightly negative
  return ok;
}

}  // namespace

void BatteryMonitor::begin() {
  // ~CHG is open drain on the module; the internal pull-up makes "not
  // charging" read HIGH even if no external pull exists.
  pinMode(BoardPins::kChargeStatus, INPUT_PULLUP);
}

bool BatteryMonitor::sample(float& volts) {
  digitalWrite(BoardPins::kBatteryEnable, LOW);  // connect the divider
  delay(2);
  int16_t raw = 0;
  const bool ok = saadcSampleAin7(raw);
  digitalWrite(BoardPins::kBatteryEnable, HIGH);  // and stop draining it
  if (ok) {
    volts = raw * kFullScaleVolts / 4096.0f * kDividerRatio;
  }
  return ok;
}

bool BatteryMonitor::pollCharging(uint64_t nowMs) {
  const bool raw = digitalRead(BoardPins::kChargeStatus) == LOW;
  if (!primed_) {
    primed_ = true;
    lastRaw_ = raw;
    chargingStable_ = raw;
    rawSinceMs_ = nowMs;
    return chargingStable_;
  }
  if (raw != lastRaw_) {
    lastRaw_ = raw;
    rawSinceMs_ = nowMs;
  } else if (raw != chargingStable_ &&
             nowMs - rawSinceMs_ >= kChargeDebounceMs) {
    chargingStable_ = raw;
  }
  return chargingStable_;
}

bool BatteryMonitor::usbPowered() const {
  return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0;
}
