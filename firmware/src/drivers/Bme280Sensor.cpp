// SPDX-License-Identifier: GPL-3.0-only
#include "Bme280Sensor.h"

#include <Adafruit_BME280.h>
#include <Wire.h>
#include <math.h>

#include "../diagnostics/Trace.h"
#include "../model/Reading.h"

namespace {

constexpr uint8_t kPrimaryAddress = 0x76;
constexpr uint8_t kAlternateAddress = 0x77;

class Driver : public Adafruit_BME280 {
 public:
  bool reinitialize() {
    return i2c_dev != nullptr && i2c_dev->begin() && init();
  }

  void triggerForcedMeasurement() {
    write8(BME280_REGISTER_CONTROL, _measReg.get());
  }

  bool isMeasuring() {
    constexpr uint8_t kMeasuringMask = 1u << 3;
    return (read8(BME280_REGISTER_STATUS) & kMeasuringMask) != 0;
  }
};

// Quiesco has exactly one BME280. Keeping its library object private to this
// translation unit prevents Adafruit types from leaking through the adapter.
Driver& driver() {
  static Driver instance;
  return instance;
}

bool validTemperature(float value) {
  return isfinite(value) && value >= -40.0f && value <= 85.0f;
}

bool validHumidity(float value) {
  return isfinite(value) && value >= 0.0f && value <= 100.0f;
}

bool validPressure(float value) {
  return isfinite(value) && value > 0.0f;
}

}  // namespace

bool Bme280Sensor::begin() {
  const uint8_t knownAddress = address_;
  address_ = 0;
  deadlineMs_ = 0;
  state_ = State::kUnavailable;

  // Reuse the library's I2C transport after the first cycle. Adafruit's
  // begin() replaces a heap-allocated transport, which is unnecessary when
  // the same physical sensor returns after a rail cycle.
  if (knownAddress != 0 && driver().reinitialize()) {
    address_ = knownAddress;
  } else if (driver().begin(kPrimaryAddress, &Wire)) {
    address_ = kPrimaryAddress;
  } else if (driver().begin(kAlternateAddress, &Wire)) {
    address_ = kAlternateAddress;
  } else {
    QUIESCO_TRACE_EVENT("bme280", "missing", 1);
    return false;
  }

  driver().setSampling(Adafruit_BME280::MODE_FORCED,
                       Adafruit_BME280::SAMPLING_X1,
                       Adafruit_BME280::SAMPLING_X1,
                       Adafruit_BME280::SAMPLING_X1,
                       Adafruit_BME280::FILTER_OFF);
  state_ = State::kIdle;
  return true;
}

void Bme280Sensor::start(uint64_t nowMs) {
  if (!present()) {
    return;
  }
  driver().triggerForcedMeasurement();
  deadlineMs_ = nowMs + kMeasurementTimeoutMs;
  state_ = State::kMeasuring;
}

void Bme280Sensor::poll(uint64_t nowMs) {
  if (state_ != State::kMeasuring) {
    return;
  }
  if (nowMs >= deadlineMs_) {
    QUIESCO_TRACE_EVENT("bme280", "timeout", 1);
    state_ = State::kTimedOut;
    return;
  }
  if (!driver().isMeasuring()) {
    state_ = State::kComplete;
  }
}

bool Bme280Sensor::ready() const {
  return state_ == State::kUnavailable || state_ == State::kComplete ||
         state_ == State::kTimedOut;
}

void Bme280Sensor::collect(Reading& reading) {
  if (state_ != State::kComplete) {
    return;
  }

  const float temperature = driver().readTemperature();
  const float humidity = driver().readHumidity();
  const float pressure = driver().readPressure();
  QUIESCO_TRACE_FLOAT("bme280", "temperature C", temperature);
  QUIESCO_TRACE_FLOAT("bme280", "humidity %", humidity);
  QUIESCO_TRACE_FLOAT("bme280", "pressure Pa", pressure);

  if (validTemperature(temperature)) {
    reading.temperatureC = temperature;
    reading.valid |= VALID_TEMPERATURE;
  }
  if (validHumidity(humidity)) {
    reading.humidityPct = humidity;
    reading.valid |= VALID_HUMIDITY;
  }
  if (validPressure(pressure)) {
    reading.pressurePa = pressure;
    reading.valid |= VALID_PRESSURE;
  }
}

void Bme280Sensor::end() {
  state_ = State::kUnavailable;
  deadlineMs_ = 0;
}

bool Bme280Sensor::present() const {
  return state_ != State::kUnavailable;
}

bool Bme280Sensor::timedOut() const {
  return state_ == State::kTimedOut;
}
