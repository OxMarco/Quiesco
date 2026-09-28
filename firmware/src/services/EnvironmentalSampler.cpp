// SPDX-License-Identifier: GPL-3.0-only
#include "EnvironmentalSampler.h"

#include <Wire.h>

#include "../model/Reading.h"
#include "../platform/I2cBusRecovery.h"

bool EnvironmentalSampler::begin() {
  // Before TWIM owns the pins: a part left mid-byte would otherwise hold SDA
  // low and every sensor on the bus would read as missing.
  busRecovered_ = I2cBusRecovery::recover();
  Wire.begin();
  Wire.setClock(100000);  // margin: every part on the bus allows 400 kHz
  busStarted_ = true;
  const bool bme280Present = bme280_.begin();
  const bool veml7700Present = veml7700_.begin();
  const bool scd41Present = scd41_.begin();
  return bme280Present || veml7700Present || scd41Present;
}

void EnvironmentalSampler::start(uint64_t nowMs) {
  pressureHandedOff_ = false;
  bme280_.start(nowMs);
  veml7700_.start(nowMs);
  scd41_.start(nowMs);
}

void EnvironmentalSampler::poll(uint64_t nowMs) {
  bme280_.poll(nowMs);
  veml7700_.poll(nowMs);
  // The BME280 finishes within milliseconds, long before the SCD41's
  // measured shot starts (~5 s in), so that shot is pressure-compensated.
  if (!pressureHandedOff_ && bme280_.ready()) {
    pressureHandedOff_ = true;
    Reading early;
    bme280_.collect(early);
    if (early.valid & VALID_PRESSURE) {
      scd41_.setAmbientPressure(early.pressurePa);
    }
  }
  scd41_.poll(nowMs);
}

bool EnvironmentalSampler::ready() const {
  return bme280_.ready() && veml7700_.ready() && scd41_.ready();
}

void EnvironmentalSampler::collect(Reading& reading) {
  bme280_.collect(reading);
  veml7700_.collect(reading);
  scd41_.collect(reading);
}

void EnvironmentalSampler::end() {
  bme280_.end();
  veml7700_.end();
  scd41_.end();
  if (busStarted_) {
    Wire.end();
    busStarted_ = false;
  }
}

bool EnvironmentalSampler::bme280Present() const {
  return bme280_.present();
}

bool EnvironmentalSampler::bme280TimedOut() const {
  return bme280_.timedOut();
}

bool EnvironmentalSampler::veml7700Present() const {
  return veml7700_.present();
}

bool EnvironmentalSampler::veml7700TimedOut() const {
  return veml7700_.timedOut();
}

bool EnvironmentalSampler::scd41Present() const {
  return scd41_.present();
}

bool EnvironmentalSampler::scd41TimedOut() const {
  return scd41_.timedOut();
}
