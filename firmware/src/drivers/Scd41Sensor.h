// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

struct Reading;

// SCD41 CO2 via the pinned Sensirion I2C SCD4x driver, in power-cycled
// single-shot operation (Sensirion "SCD4x Low Power Operation" AN, §2.4 and
// §4.4): the rail powers the sensor for each cycle, and after power-up the
// first single shot only stabilises the sensor, so its reading is discarded
// and the second one is used (~10 s in all). The driver's measureSingleShot()
// blocks for 5 s, so the command is sent directly and completion is polled.
// ASC is not supported in this mode (see disableAutomaticSelfCalibration).
class Scd41Sensor {
 public:
  // A single shot takes 5 s (datasheet); the sensor ignores commands until
  // then, so data-ready is polled only afterwards, sparingly.
  static constexpr uint32_t kSingleShotMs = 5000;
  static constexpr uint32_t kDataReadyPollIntervalMs = 100;
  static constexpr uint32_t kSingleShotTimeoutMs = 7000;
  // FRC run-up for power-cycled use (AN §5.1): single shots at the field
  // sampling rate of 1 min for 5 min, then FRC from idle.
  static constexpr uint32_t kFrcRunUpShotIntervalMs = 60000;
  static constexpr uint8_t kFrcRunUpShots = 5;

  bool begin();
  void start(uint64_t nowMs);
  void poll(uint64_t nowMs);
  bool ready() const;
  void collect(Reading& reading);
  void end();

  // Pressure compensation for the measured shot, from the BME280. Takes
  // effect if it arrives before the second shot starts.
  void setAmbientPressure(float pressurePa);

  bool present() const;
  bool timedOut() const;
  // 48-bit serial from the last successful probe; 0 if never detected. Kept
  // across rail cycles so support can identify the part while it is off.
  uint64_t serialNumber() const { return serialNumber_; }
  // True once this boot has confirmed ASC is off in the sensor's EEPROM.
  bool ascDisabled() const { return ascChecked_; }

  // Forced recalibration, after collect(): startFrcRunUp() then
  // pollFrcRunUp() until it returns true (~5 min), then
  // performForcedRecalibration(), which blocks ~400 ms in the driver.
  void startFrcRunUp(uint64_t nowMs);
  enum class FrcProgress : uint8_t { kRunning, kReady, kFailed };
  FrcProgress pollFrcRunUp(uint64_t nowMs);
  bool performForcedRecalibration(uint16_t targetPpm, int16_t& correctionPpm);

 private:
  bool disableAutomaticSelfCalibration();
  bool sendSingleShot();

  enum class State : uint8_t {
    kUnavailable,
    kIdle,
    kStabilizing,  // first shot after power-up, reading discarded
    kMeasuring,    // the shot that is used
    kComplete,
    kTimedOut,
    kFrcRunUp,
  };

  State state_ = State::kUnavailable;
  uint64_t timeoutAtMs_ = 0;
  uint64_t nextProbeAtMs_ = 0;
  uint64_t serialNumber_ = 0;
  uint16_t pendingPressureHpa_ = 0;  // 0 = none
  uint8_t runUpShotsSent_ = 0;
  uint64_t runUpEndMs_ = 0;
  bool runUpShotPending_ = false;
  bool frcPrepared_ = false;
  uint64_t runUpReadAtMs_ = 0;
  bool ascChecked_ = false;
};
