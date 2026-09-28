// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

// Which detail layout the panel shows between measurements. The battery
// screen is not selectable: it overrides whenever charging or nearly empty.
enum DisplayScreen : uint8_t {
  DISPLAY_SCREEN_FACE = 0,
  DISPLAY_SCREEN_LEDGER = 1,
  DISPLAY_SCREEN_BENTO = 2,
};

// How the panel writes temperatures. Readings, offsets and comfort bands stay
// in °C; this only changes the digits drawn.
enum TemperatureUnit : uint8_t {
  TEMPERATURE_UNIT_CELSIUS = 0,
  TEMPERATURE_UNIT_FAHRENHEIT = 1,
};

struct Config {
  // Bumped with no record-layout change: isValidConfig rejects any other
  // version, so this is what discards a stored config and lets a changed
  // defaultConfig() take effect on an already-provisioned device. Every field
  // reverts to its default on the next boot, calibration offsets included.
  // Only worth spending while the default screen is still moving; once the UI
  // settles, change screens over BLE instead of wiping every other setting.
  static constexpr uint32_t kCurrentVersion = 5;
  static constexpr uint32_t kDefaultIntervalSeconds = 300;
  static constexpr uint32_t kDefaultFullRefreshEveryCycles = 10;
  static constexpr uint32_t kMaximumFullRefreshEveryCycles = 1000;
  static constexpr uint32_t kDeviceNameMaxLength = 16;
  // Calibration offset sanity bounds: anything larger points at a broken app
  // or sensor, not a legitimate correction.
  static constexpr float kMaxNoiseOffsetDb = 24.0f;
  static constexpr float kMaxTempOffsetC = 8.0f;
  static constexpr float kMaxHumidityOffsetRh = 20.0f;

  uint32_t version;
  uint32_t measurementIntervalSeconds;
  uint32_t fullRefreshEveryCycles;
  bool bleAlwaysAvailable;
  uint8_t displayScreen;
  uint8_t temperatureUnit;
  char deviceName[kDeviceNameMaxLength + 1];  // NUL-terminated UTF-8
  float noiseOffsetDb;
  float tempOffsetC;
  float humidityOffsetRh;
  // Last CO2 forced recalibration, for support. The correction itself lives
  // in the SCD41's EEPROM; these fields only record it. frcTargetPpm 0 means
  // no FRC recorded, frcEpochSeconds 0 that the clock was not synced then.
  // Not a setting: never reset by factory reset, never written over BLE.
  uint32_t frcEpochSeconds;
  uint16_t frcTargetPpm;
  int16_t frcCorrectionPpm;
};

inline Config defaultConfig() {
  Config config = {};
  config.version = Config::kCurrentVersion;
  config.measurementIntervalSeconds = Config::kDefaultIntervalSeconds;
  config.fullRefreshEveryCycles = Config::kDefaultFullRefreshEveryCycles;
  config.bleAlwaysAvailable = true;
  config.displayScreen = DISPLAY_SCREEN_BENTO;
  config.temperatureUnit = TEMPERATURE_UNIT_CELSIUS;
  strncpy(config.deviceName, "Quiesco", sizeof config.deviceName - 1);
  config.noiseOffsetDb = 0.0f;
  config.tempOffsetC = 0.0f;
  config.humidityOffsetRh = 0.0f;
  config.frcEpochSeconds = 0;
  config.frcTargetPpm = 0;
  config.frcCorrectionPpm = 0;
  return config;
}

inline bool isValidMeasurementInterval(uint32_t intervalSeconds) {
  return intervalSeconds == 60 || intervalSeconds == 300 ||
         intervalSeconds == 600 || intervalSeconds == 1800;
}

inline bool isValidDisplayScreen(uint8_t screen) {
  return screen <= DISPLAY_SCREEN_BENTO;
}

inline bool isValidTemperatureUnit(uint8_t unit) {
  return unit <= TEMPERATURE_UNIT_FAHRENHEIT;
}

// 1..16 bytes, NUL-terminated, no ASCII control bytes; multi-byte UTF-8
// (>= 0x80) passes through untouched.
inline bool isValidDeviceName(const char* name) {
  const size_t length = strnlen(name, Config::kDeviceNameMaxLength + 1);
  if (length == 0 || length > Config::kDeviceNameMaxLength) {
    return false;
  }
  for (size_t i = 0; i < length; i++) {
    const unsigned char byte = static_cast<unsigned char>(name[i]);
    if (byte < 0x20 || byte == 0x7F) {
      return false;
    }
  }
  return true;
}

// NaN fails every comparison, so non-finite offsets are rejected implicitly.
inline bool isValidCalibrationOffsets(float noiseOffsetDb, float tempOffsetC,
                                      float humidityOffsetRh) {
  return fabsf(noiseOffsetDb) <= Config::kMaxNoiseOffsetDb &&
         fabsf(tempOffsetC) <= Config::kMaxTempOffsetC &&
         fabsf(humidityOffsetRh) <= Config::kMaxHumidityOffsetRh;
}

inline bool isValidConfig(const Config& config) {
  return config.version == Config::kCurrentVersion &&
         isValidMeasurementInterval(config.measurementIntervalSeconds) &&
         config.fullRefreshEveryCycles > 0 &&
         config.fullRefreshEveryCycles <=
             Config::kMaximumFullRefreshEveryCycles &&
         isValidDisplayScreen(config.displayScreen) &&
         isValidTemperatureUnit(config.temperatureUnit) &&
         isValidDeviceName(config.deviceName) &&
         isValidCalibrationOffsets(config.noiseOffsetDb, config.tempOffsetC,
                                   config.humidityOffsetRh);
}
