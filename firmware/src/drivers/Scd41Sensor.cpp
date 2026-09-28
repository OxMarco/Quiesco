// SPDX-License-Identifier: GPL-3.0-only
#include "Scd41Sensor.h"

#include <SensirionI2cScd4x.h>
#include <Wire.h>

#include "../diagnostics/Trace.h"
#include "../model/Reading.h"

namespace {

constexpr uint8_t kAddress = SCD41_I2C_ADDR_62;
constexpr uint16_t kMeasureSingleShot = 0x219D;

SensirionI2cScd4x& driver() {
  static SensirionI2cScd4x instance;
  return instance;
}

// Presence is a real command, not an empty Wire transaction: on this mbed core
// beginTransmission()/endTransmission() with no payload is sent as a 1-byte
// *read*, and the SCD41 NAKs its read header whenever it has nothing queued.
// That probe reported the part missing on every cycle while it was fine.
// get_serial_number is idle-mode only, which the rail cycle guarantees, and
// its ~1 ms execution time fits well inside the power-up settle.
bool probe(uint64_t& serialNumber) {
  driver().begin(Wire, kAddress);
  const int16_t error = driver().getSerialNumber(serialNumber);
  if (error != 0) {
    QUIESCO_TRACE_EVENT("scd41", "probe error", error);
  }
  return error == 0;
}

}  // namespace

bool Scd41Sensor::begin() {
  frcPrepared_ = false;
  state_ = State::kUnavailable;
  timeoutAtMs_ = 0;
  nextProbeAtMs_ = 0;

  uint64_t serialNumber = 0;
  if (!probe(serialNumber)) {
    return false;
  }
  serialNumber_ = serialNumber;
  state_ = State::kIdle;
  if (!ascChecked_) {
    ascChecked_ = disableAutomaticSelfCalibration();
  }
  return true;
}

void Scd41Sensor::start(uint64_t nowMs) {
  if (!present()) {
    return;
  }
  pendingPressureHpa_ = 0;
  if (!sendSingleShot()) {
    QUIESCO_TRACE_EVENT("scd41", "single shot failed", 1);
    state_ = State::kTimedOut;
    return;
  }
  nextProbeAtMs_ = nowMs + kSingleShotMs;
  timeoutAtMs_ = nowMs + kSingleShotTimeoutMs;
  state_ = State::kStabilizing;
}

void Scd41Sensor::poll(uint64_t nowMs) {
  if (state_ != State::kStabilizing && state_ != State::kMeasuring) {
    return;
  }
  if (nowMs >= timeoutAtMs_) {
    QUIESCO_TRACE_EVENT("scd41", "timeout state", static_cast<uint8_t>(state_));
    state_ = State::kTimedOut;
    return;
  }
  if (nowMs < nextProbeAtMs_) {
    return;
  }
  nextProbeAtMs_ = nowMs + kDataReadyPollIntervalMs;
  bool dataReady = false;
  const int16_t readyError = driver().getDataReadyStatus(dataReady);
  if (readyError != 0) {
    QUIESCO_TRACE_EVENT("scd41", "data ready error", readyError);
  }
  if (readyError != 0 || !dataReady) {
    return;
  }
  if (state_ == State::kMeasuring) {
    state_ = State::kComplete;
    return;
  }
  // Stabilising shot done: read it to clear data-ready, drop the value, set
  // this cycle's pressure, and start the shot that counts.
  uint16_t co2Ppm = 0;
  float temperatureC = 0.0f, humidityPct = 0.0f;
  driver().readMeasurement(co2Ppm, temperatureC, humidityPct);
  if (pendingPressureHpa_ != 0) {
    driver().setAmbientPressure(static_cast<uint32_t>(pendingPressureHpa_) *
                                100);
  }
  if (!sendSingleShot()) {
    QUIESCO_TRACE_EVENT("scd41", "single shot failed", 2);
    state_ = State::kTimedOut;
    return;
  }
  nextProbeAtMs_ = nowMs + kSingleShotMs;
  timeoutAtMs_ = nowMs + kSingleShotTimeoutMs;
  state_ = State::kMeasuring;
}

void Scd41Sensor::setAmbientPressure(float pressurePa) {
  // Datasheet range 700-1200 hPa; anything else is a bad BME280 value.
  const float hpa = pressurePa / 100.0f;
  if (hpa >= 700.0f && hpa <= 1200.0f) {
    pendingPressureHpa_ = static_cast<uint16_t>(hpa + 0.5f);
  }
}

bool Scd41Sensor::sendSingleShot() {
  uint8_t buffer[2];
  SensirionI2CTxFrame frame =
      SensirionI2CTxFrame::createWithUInt16Command(kMeasureSingleShot, buffer,
                                                   sizeof buffer);
  return SensirionI2CCommunication::sendFrame(kAddress, frame, Wire) == 0;
}

bool Scd41Sensor::ready() const {
  return state_ == State::kUnavailable || state_ == State::kComplete ||
         state_ == State::kTimedOut;
}

void Scd41Sensor::collect(Reading& reading) {
  if (state_ != State::kComplete) {
    return;
  }
  uint16_t co2Ppm = 0;
  float temperatureC = 0.0f, humidityPct = 0.0f;
  const int16_t error =
      driver().readMeasurement(co2Ppm, temperatureC, humidityPct);
  if (error != 0) {
    QUIESCO_TRACE_EVENT("scd41", "read error", error);
    return;
  }
  QUIESCO_TRACE_FLOAT("scd41", "co2 ppm", co2Ppm);
  if (co2Ppm != 0) {  // 0 ppm is the sensor's invalid-sample marker
    reading.co2Ppm = co2Ppm;
    reading.valid |= VALID_CO2;
  }
  // Temperature and humidity come from the BME280. The SCD41's copies are
  // recorded only for comparison: its emitter heats them during the shot.
  reading.scdTemperatureC = temperatureC;
  reading.scdHumidityPct = humidityPct;
  reading.scdRhtValid = true;
}

void Scd41Sensor::end() {
  // The rail cut powers the sensor down; single-shot mode leaves nothing
  // running to stop.
  frcPrepared_ = false;
  state_ = State::kUnavailable;
  timeoutAtMs_ = 0;
  nextProbeAtMs_ = 0;
}

bool Scd41Sensor::present() const {
  return state_ != State::kUnavailable;
}

bool Scd41Sensor::timedOut() const {
  return state_ == State::kTimedOut;
}

// ASC needs the sensor measuring continuously for days, which a sensor
// powered for five seconds per cycle never does; left on, it would learn from
// those fragments. The enable flag lives in RAM, so it is persisted: the
// EEPROM write (800 ms, ~2000-write endurance) happens once per sensor, and
// every later boot only reads the flag back. Idle mode only, which begin()
// guarantees.
bool Scd41Sensor::disableAutomaticSelfCalibration() {
  uint16_t enabled = 1;
  if (driver().getAutomaticSelfCalibrationEnabled(enabled) != 0) {
    return false;
  }
  if (enabled != 0 &&
      (driver().setAutomaticSelfCalibrationEnabled(0) != 0 ||
       driver().persistSettings() != 0)) {
    return false;
  }
  return true;
}

void Scd41Sensor::startFrcRunUp(uint64_t nowMs) {
  if (!present()) {
    return;
  }
  frcPrepared_ = false;
  runUpShotPending_ = false;
  runUpShotsSent_ = 0;
  nextProbeAtMs_ = nowMs;  // first run-up shot at once
  runUpEndMs_ = nowMs + kFrcRunUpShots * kFrcRunUpShotIntervalMs;
  state_ = State::kFrcRunUp;
}

Scd41Sensor::FrcProgress Scd41Sensor::pollFrcRunUp(uint64_t nowMs) {
  if (state_ != State::kFrcRunUp) {
    return frcPrepared_ ? FrcProgress::kReady : FrcProgress::kFailed;
  }
  if (runUpShotPending_ && nowMs >= runUpReadAtMs_) {
    bool dataReady = false;
    uint16_t co2Ppm = 0;
    float temperatureC = 0.0f, humidityPct = 0.0f;
    if (nowMs >= timeoutAtMs_) {
      state_ = State::kTimedOut;
      return FrcProgress::kFailed;
    }
    runUpReadAtMs_ = nowMs + kDataReadyPollIntervalMs;
    if (driver().getDataReadyStatus(dataReady) != 0 || !dataReady) {
      return FrcProgress::kRunning;
    }
    if (driver().readMeasurement(co2Ppm, temperatureC, humidityPct) != 0 ||
        co2Ppm == 0) {
      state_ = State::kTimedOut;
      return FrcProgress::kFailed;
    }
    runUpShotPending_ = false;
    ++runUpShotsSent_;  // only completed, valid measurements count
  }
  if (!runUpShotPending_ && runUpShotsSent_ < kFrcRunUpShots &&
      nowMs >= nextProbeAtMs_) {
    if (!sendSingleShot()) {
      state_ = State::kTimedOut;
      return FrcProgress::kFailed;
    }
    runUpShotPending_ = true;
    runUpReadAtMs_ = nowMs + kSingleShotMs;
    timeoutAtMs_ = nowMs + kSingleShotTimeoutMs;
    nextProbeAtMs_ = nowMs + kFrcRunUpShotIntervalMs;
  }
  if (nowMs >= runUpEndMs_ && runUpShotsSent_ == kFrcRunUpShots &&
      !runUpShotPending_) {
    state_ = State::kIdle;
    frcPrepared_ = true;
    return FrcProgress::kReady;
  }
  return FrcProgress::kRunning;
}

bool Scd41Sensor::performForcedRecalibration(uint16_t targetPpm,
                                             int16_t& correctionPpm) {
  if (!frcPrepared_ || state_ != State::kIdle) {
    return false;
  }
  frcPrepared_ = false;  // preparation authorizes exactly one attempt
  // The Sensirion driver waits the required 400 ms internally. 0xFFFF is the
  // sensor's FRC-failed marker; otherwise the correction is offset by 0x8000.
  uint16_t raw = 0;
  if (driver().performForcedRecalibration(targetPpm, raw) != 0 ||
      raw == 0xFFFF) {
    return false;
  }
  correctionPpm = static_cast<int16_t>(static_cast<int32_t>(raw) - 0x8000);
  return true;
}

