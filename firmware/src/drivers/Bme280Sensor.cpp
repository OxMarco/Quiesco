// SPDX-License-Identifier: GPL-3.0-only
#include "Bme280Sensor.h"

#include <Adafruit_BME280.h>
#include <Wire.h>
#include <math.h>

#include "../diagnostics/Trace.h"
#include "Bme280Compensation.h"
#include "../model/Reading.h"

namespace {

constexpr uint8_t kPrimaryAddress = 0x76;
constexpr uint8_t kAlternateAddress = 0x77;

constexpr uint8_t kChipId = 0x60;
// Status polls after the soft reset, 2 ms apart: the datasheet's start-up
// time is 2 ms, so 10 leaves a wide margin.
constexpr uint8_t kResetPolls = 10;

// Adafruit's begin() and init() are replaced: init() polls the NVM-copy flag
// with no limit, and a failed I2C read returns the register address (0xF3,
// bit 0 set), so a sensor that stops answering after the soft reset hung the
// cycle until the watchdog reset the unit. init() also costs 110 ms of
// delay() every cycle and rereads the calibration, which is fixed per chip.
class Driver : public Adafruit_BME280 {
 public:
  // A new address: a new transport and the chip's calibration.
  bool attach(uint8_t address) {
    delete i2c_dev;
    i2c_dev = new Adafruit_I2CDevice(address, &Wire);
    address_ = address;
    if (!acknowledges() || !reset()) {
      return false;
    }
    readCoefficients();
    copyCalibration();
    if (!Bme280::plausible(calibration_)) {
      QUIESCO_TRACE_EVENT("bme280", "calibration unreadable", 1);
      return false;
    }
    return true;
  }

  // The same sensor after a rail cycle: the transport and the calibration
  // read by attach() still apply.
  bool reattach() {
    return i2c_dev != nullptr && acknowledges() && reset();
  }

  void triggerForcedMeasurement() {
    write8(BME280_REGISTER_CONTROL, _measReg.get());
  }

  // A failed status read counts as still measuring: read8() would return
  // the register address, 0xF3, whose bit 3 is clear ("done").
  bool isMeasuring() {
    constexpr uint8_t kMeasuringMask = 1u << 3;
    uint8_t status = 0;
    return !readRegisters(BME280_REGISTER_STATUS, &status, 1) ||
           (status & kMeasuringMask) != 0;
  }

  // One checked burst of every data register, so the three channels come
  // from the same measurement and a failed transfer is never decoded.
  bool readMeasurement(Bme280::Measurement& out) {
    uint8_t burst[8];
    if (!readRegisters(BME280_REGISTER_PRESSUREDATA, burst, sizeof burst)) {
      QUIESCO_TRACE_EVENT("bme280", "read failed", 1);
      return false;
    }
    out = Bme280::compensate(calibration_, Bme280::decodeBurst(burst));
    return true;
  }

 private:
  // Soft reset (IIR off, sleep mode), then a bounded wait for the NVM copy.
  bool reset() {
    if (read8(BME280_REGISTER_CHIPID) != kChipId) {
      return false;
    }
    write8(BME280_REGISTER_SOFTRESET, 0xB6);
    for (uint8_t poll = 0; poll < kResetPolls; ++poll) {
      delay(2);
      if (!isReadingCalibration()) {
        return true;
      }
    }
    QUIESCO_TRACE_EVENT("bme280", "reset timeout", 1);
    return false;
  }

  // The address probe Adafruit_I2CDevice::begin() did, without its
  // Wire.begin(): on this core that allocates a new mbed::I2C and drops the
  // one EnvironmentalSampler began, so every cycle leaked ~200 bytes of heap
  // (31 to 39 KiB over 40 cycles on the bench unit) until, after a day or
  // two, the unit stalled and the watchdog reset it. The bus is already
  // begun; BusIO's reads and writes do not need its begin().
  bool acknowledges() {
    Wire.beginTransmission(address_);
    Wire.write(0);  // as BusIO on mbed: a write request, not a read
    return Wire.endTransmission() == 0;
  }

  bool readRegisters(uint8_t reg, uint8_t* out, size_t length) {
    return i2c_dev != nullptr && i2c_dev->write_then_read(&reg, 1, out, length);
  }

  void copyCalibration() {
    const auto& c = _bme280_calib;
    calibration_ = {c.dig_T1, c.dig_T2, c.dig_T3, c.dig_P1, c.dig_P2,
                    c.dig_P3, c.dig_P4, c.dig_P5, c.dig_P6, c.dig_P7,
                    c.dig_P8, c.dig_P9, c.dig_H1, c.dig_H2, c.dig_H3,
                    c.dig_H4, c.dig_H5, c.dig_H6};
  }

  Bme280::Calibration calibration_ = {};
  uint8_t address_ = 0;
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


}  // namespace

bool Bme280Sensor::begin() {
  const uint8_t knownAddress = address_;
  address_ = 0;
  deadlineMs_ = 0;
  state_ = State::kUnavailable;

  // Reuse the I2C transport after the first cycle: attach() replaces a
  // heap-allocated one, which is unnecessary when the same physical sensor
  // returns after a rail cycle.
  if (knownAddress != 0 && driver().reattach()) {
    address_ = knownAddress;
  } else if (driver().attach(kPrimaryAddress)) {
    address_ = kPrimaryAddress;
  } else if (driver().attach(kAlternateAddress)) {
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

  Bme280::Measurement measured;
  if (!driver().readMeasurement(measured)) {
    return;
  }
  const float temperature = measured.temperatureC;
  const float humidity = measured.humidityPct;
  const float pressure = measured.pressurePa;
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
  if (Bme280::validPressurePa(pressure)) {
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
