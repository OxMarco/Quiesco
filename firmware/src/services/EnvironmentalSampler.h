// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

#include "../drivers/Bme280Sensor.h"
#include "../drivers/Scd41Sensor.h"
#include "../drivers/Veml7700Sensor.h"

struct Reading;

class EnvironmentalSampler {
 public:
  bool begin();
  void start(uint64_t nowMs);
  void poll(uint64_t nowMs);
  bool ready() const;
  void collect(Reading& reading);
  void end();

  bool bme280Present() const;
  bool bme280TimedOut() const;
  bool veml7700Present() const;
  bool veml7700TimedOut() const;
  bool scd41Present() const;
  bool scd41TimedOut() const;
  uint64_t scd41Serial() const { return scd41_.serialNumber(); }
  bool scd41AscDisabled() const { return scd41_.ascDisabled(); }
  // False when SDA was still low after bus recovery this cycle: a part is
  // holding the bus, so every I2C sensor will read as missing.
  bool busFree() const { return busRecovered_; }
  // FRC passthrough (see Scd41Sensor); keeps the sensor object private.
  void scd41StartFrcRunUp(uint64_t nowMs) { scd41_.startFrcRunUp(nowMs); }
  Scd41Sensor::FrcProgress scd41PollFrcRunUp(uint64_t nowMs) { return scd41_.pollFrcRunUp(nowMs); }
  bool scd41ForcedRecalibration(uint16_t targetPpm, int16_t& correctionPpm) {
    return scd41_.performForcedRecalibration(targetPpm, correctionPpm);
  }

 private:
  Bme280Sensor bme280_;
  Veml7700Sensor veml7700_;
  Scd41Sensor scd41_;
  bool busStarted_ = false;
  bool pressureHandedOff_ = false;
  bool busRecovered_ = true;
};
