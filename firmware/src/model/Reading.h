// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

enum ReadingValid : uint32_t {
  VALID_TEMPERATURE = 1u << 0,
  VALID_HUMIDITY = 1u << 1,
  VALID_PRESSURE = 1u << 2,
  VALID_CO2 = 1u << 3,
  VALID_LIGHT = 1u << 4,
  VALID_NOISE = 1u << 5,
  VALID_BATTERY = 1u << 6,
};

struct Reading {
  uint64_t monotonicMs = 0;
  uint64_t epochMs = 0;  // wall-clock at capture; 0 until a BLE time sync
  float temperatureC = 0.0f;
  float humidityPct = 0.0f;
  float pressurePa = 0.0f;
  float co2Ppm = 0.0f;
  float lux = 0.0f;
  float noiseDb = 0.0f;
  float batteryV = 0.0f;
  uint32_t valid = 0;
  // The SCD41's own temperature and humidity, read with each CO2 value. Not
  // a displayed reading: the sensor heats itself during a measurement. Kept
  // for comparison with the BME280 (log dump only, not sent over BLE).
  float scdTemperatureC = 0.0f;
  float scdHumidityPct = 0.0f;
  bool scdRhtValid = false;
};
