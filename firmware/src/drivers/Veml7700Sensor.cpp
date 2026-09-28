// SPDX-License-Identifier: GPL-3.0-only
#include "Veml7700Sensor.h"

#include <Adafruit_VEML7700.h>
#include <Wire.h>
#include <math.h>

#include "../diagnostics/Trace.h"
#include "../model/Reading.h"

namespace {

constexpr uint8_t kAddress = VEML7700_I2CADDR_DEFAULT;
constexpr uint16_t kShutdown = 1u << 0;  // ALS_SD

// Plain Wire transfers with their status checked; the Adafruit library is
// kept for its register names only. Its register reads return 0xFFFF on a
// failed transfer: readALS() handed that back as counts, and its
// read-modify-write setters wrote it back as config bits (shutdown, invalid
// integration time). 0xFFFF counts take the bright path and come out above
// 200 klx, which collect() drops: the only way this driver can end a
// measurement in veml=read-error, as every cycle did on 2026-09-27.
// Registers are now written whole and a failed read is retried.
bool writeRegister(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(kAddress);
  Wire.write(reg);
  Wire.write(static_cast<uint8_t>(value & 0xFF));
  Wire.write(static_cast<uint8_t>(value >> 8));
  const uint8_t status = Wire.endTransmission();
  if (status != 0) {
    QUIESCO_TRACE_EVENT("veml", "write error", status);
  }
  return status == 0;
}

bool readRegister(uint8_t reg, uint16_t& value) {
  Wire.beginTransmission(kAddress);
  Wire.write(reg);
  const uint8_t status = Wire.endTransmission(false);
  if (status != 0) {
    QUIESCO_TRACE_EVENT("veml", "read address error", status);
    return false;
  }
  const uint8_t received = Wire.requestFrom(kAddress, static_cast<uint8_t>(2));
  if (received != 2) {
    QUIESCO_TRACE_EVENT("veml", "read short bytes", received);
    return false;
  }
  const uint8_t lsb = static_cast<uint8_t>(Wire.read());
  const uint8_t msb = static_cast<uint8_t>(Wire.read());
  value = static_cast<uint16_t>(lsb | (msb << 8));
  return true;
}

uint16_t configValue(uint8_t gain, uint8_t integrationTime) {
  // Persistence 1, interrupt off.
  return static_cast<uint16_t>((gain & 0x03) << 11 |
                               (integrationTime & 0x0F) << 6);
}

// Shut down, write the new settings, power up (Vishay: >= 2.5 ms before the
// first integration starts). Settings apply from the next integration.
bool configure(uint8_t gain, uint8_t integrationTime) {
  const uint16_t value = configValue(gain, integrationTime);
  if (!writeRegister(VEML7700_ALS_CONFIG, value | kShutdown) ||
      !writeRegister(VEML7700_ALS_CONFIG, value)) {
    return false;
  }
  delay(5);
  return true;
}

// Current datasheet (rev 1.8): 0.0042 lx/count at gain 2 and 800 ms,
// scaling inversely with gain and integration time.
constexpr float kFinestResolutionLux = 0.0042f;
constexpr uint16_t kLowCounts = 100;
constexpr uint16_t kHighCounts = 10000;

float resolutionLux(float gain, float integrationMs) {
  return kFinestResolutionLux * (800.0f / integrationMs) * (2.0f / gain);
}

// Vishay's non-linearity correction for the low gains (application note
// 84323), in lux.
float correctedLux(float lux) {
  return (((6.0135e-13f * lux - 9.3924e-9f) * lux + 8.1488e-5f) * lux +
          1.0023f) *
         lux;
}

bool validLux(float value) {
  constexpr float kMaximumLux = 200000.0f;
  return isfinite(value) && value >= 0.0f && value <= kMaximumLux;
}

}  // namespace

bool Veml7700Sensor::begin() {
  state_ = State::kUnavailable;
  readyAtMs_ = 0;
  timeoutAtMs_ = 0;

  // An ACKed write is the probe; power save off, coarse range.
  if (!writeRegister(VEML7700_ALS_POWER_SAVE, 0) ||
      !configure(VEML7700_GAIN_1_8, VEML7700_IT_100MS)) {
    QUIESCO_TRACE_EVENT("veml", "missing", 1);
    return false;
  }
  state_ = State::kIdle;
  return true;
}

void Veml7700Sensor::start(uint64_t nowMs) {
  if (!present()) {
    return;
  }
  readyAtMs_ = nowMs + kCoarseWaitMs;
  timeoutAtMs_ = nowMs + kMeasurementTimeoutMs;
  state_ = State::kCoarse;
}

void Veml7700Sensor::poll(uint64_t nowMs) {
  if (state_ != State::kCoarse && state_ != State::kFine &&
      state_ != State::kBright) {
    return;
  }
  if (nowMs >= timeoutAtMs_) {
    QUIESCO_TRACE_EVENT("veml", "timeout state", static_cast<uint8_t>(state_));
    state_ = State::kTimedOut;
    return;
  }
  if (nowMs < readyAtMs_) {
    return;
  }
  // A failed transfer is retried until the timeout, never read as counts.
  uint16_t counts = 0;
  if (!readRegister(VEML7700_ALS_DATA, counts)) {
    QUIESCO_TRACE_EVENT("veml", "read retry state", static_cast<uint8_t>(state_));
    readyAtMs_ = nowMs + kRetryMs;
    return;
  }
  QUIESCO_TRACE_EVENT("veml",
                      state_ == State::kCoarse ? "coarse counts"
                      : state_ == State::kFine ? "fine counts"
                                               : "bright counts",
                      counts);
  switch (state_) {
    case State::kCoarse:
      if (counts <= kLowCounts) {
        if (!configure(VEML7700_GAIN_2, VEML7700_IT_400MS)) {
          readyAtMs_ = nowMs + kRetryMs;  // still coarse: re-read, re-range
          break;
        }
        readyAtMs_ = nowMs + kFineWaitMs;
        state_ = State::kFine;
      } else if (counts > kHighCounts) {
        if (!configure(VEML7700_GAIN_1_8, VEML7700_IT_25MS)) {
          readyAtMs_ = nowMs + kRetryMs;
          break;
        }
        readyAtMs_ = nowMs + kBrightWaitMs;
        state_ = State::kBright;
      } else {
        lux_ = correctedLux(counts * resolutionLux(0.125f, 100.0f));
        state_ = State::kComplete;
      }
      break;
    case State::kFine:
      lux_ = counts * resolutionLux(2.0f, 400.0f);  // linear at high gain
      state_ = State::kComplete;
      break;
    default:  // kBright
      lux_ = correctedLux(counts * resolutionLux(0.125f, 25.0f));
      state_ = State::kComplete;
      break;
  }
}

bool Veml7700Sensor::ready() const {
  return state_ == State::kUnavailable || state_ == State::kComplete ||
         state_ == State::kTimedOut;
}

void Veml7700Sensor::collect(Reading& reading) {
  if (state_ != State::kComplete) {
    return;
  }

  QUIESCO_TRACE_FLOAT("veml", "lux", lux_);
  if (validLux(lux_)) {
    reading.lux = lux_;
    reading.valid |= VALID_LIGHT;
  }
}

void Veml7700Sensor::end() {
  if (present()) {
    writeRegister(VEML7700_ALS_CONFIG, kShutdown);  // the rail drops anyway
  }
  state_ = State::kUnavailable;
  readyAtMs_ = 0;
  timeoutAtMs_ = 0;
}

bool Veml7700Sensor::present() const {
  return state_ != State::kUnavailable;
}

bool Veml7700Sensor::timedOut() const {
  return state_ == State::kTimedOut;
}

