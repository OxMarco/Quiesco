// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <math.h>
#include <stdint.h>

// The BME280's raw burst (0xF7-0xFE) and Bosch's integer compensation
// (datasheet §4.2.3, as in Adafruit_BME280 2.3.0), kept free of Arduino types
// so the host tests can run it. Adafruit's own read*() functions reread the
// registers without checking the I2C transfer: a failed read decoded the
// register address and stack bytes as a plausible reading.
namespace Bme280 {

struct Calibration {
  uint16_t t1;
  int16_t t2, t3;
  uint16_t p1;
  int16_t p2, p3, p4, p5, p6, p7, p8, p9;
  uint8_t h1;
  int16_t h2;
  uint8_t h3;
  int16_t h4, h5;
  int8_t h6;
};

// What the data registers hold after a reset, and for a channel whose
// measurement was skipped.
constexpr int32_t kSkipped20Bit = 0x80000;
constexpr int32_t kSkipped16Bit = 0x8000;

struct Raw {
  int32_t pressure;     // 20 bit
  int32_t temperature;  // 20 bit
  int32_t humidity;     // 16 bit
};

inline Raw decodeBurst(const uint8_t burst[8]) {
  Raw raw;
  raw.pressure = (int32_t{burst[0]} << 12) | (int32_t{burst[1]} << 4) |
                 (burst[2] >> 4);
  raw.temperature = (int32_t{burst[3]} << 12) | (int32_t{burst[4]} << 4) |
                    (burst[5] >> 4);
  raw.humidity = (int32_t{burst[6]} << 8) | burst[7];
  return raw;
}

// A calibration read from a bus that did not answer is all zeros or the
// register address repeated; T1 and P1 are never zero on a real chip.
inline bool plausible(const Calibration& c) { return c.t1 != 0 && c.p1 != 0; }

// t_fine, the temperature term pressure and humidity are compensated with.
inline int32_t temperatureFine(const Calibration& c, int32_t adcT) {
  int32_t var1 = (adcT / 8) - (int32_t{c.t1} * 2);
  var1 = (var1 * int32_t{c.t2}) / 2048;
  int32_t var2 = (adcT / 16) - int32_t{c.t1};
  var2 = (((var2 * var2) / 4096) * int32_t{c.t3}) / 16384;
  return var1 + var2;
}

inline float temperatureC(int32_t tFine) {
  return static_cast<float>((tFine * 5 + 128) / 256) / 100.0f;
}

inline float pressurePa(const Calibration& c, int32_t tFine, int32_t adcP) {
  int64_t var1 = int64_t{tFine} - 128000;
  int64_t var2 = var1 * var1 * int64_t{c.p6};
  var2 = var2 + ((var1 * int64_t{c.p5}) * 131072);
  var2 = var2 + (int64_t{c.p4} * 34359738368LL);
  var1 = ((var1 * var1 * int64_t{c.p3}) / 256) + (var1 * int64_t{c.p2} * 4096);
  const int64_t var3 = int64_t{1} * 140737488355328LL;
  var1 = (var3 + var1) * int64_t{c.p1} / 8589934592LL;
  if (var1 == 0) {
    return NAN;
  }
  int64_t var4 = 1048576 - adcP;
  var4 = (((var4 * 2147483648LL) - var2) * 3125) / var1;
  var1 = (int64_t{c.p9} * (var4 / 8192) * (var4 / 8192)) / 33554432;
  var2 = (int64_t{c.p8} * var4) / 524288;
  var4 = ((var4 + var1 + var2) / 256) + (int64_t{c.p7} * 16);
  return static_cast<float>(var4) / 256.0f;
}

inline float humidityPct(const Calibration& c, int32_t tFine, int32_t adcH) {
  int32_t var1 = tFine - 76800;
  int32_t var2 = adcH * 16384;
  int32_t var3 = int32_t{c.h4} * 1048576;
  int32_t var4 = int32_t{c.h5} * var1;
  int32_t var5 = (((var2 - var3) - var4) + 16384) / 32768;
  var2 = (var1 * int32_t{c.h6}) / 1024;
  var3 = (var1 * int32_t{c.h3}) / 2048;
  var4 = ((var2 * (var3 + 32768)) / 1024) + 2097152;
  var2 = ((var4 * int32_t{c.h2}) + 8192) / 16384;
  var3 = var5 * var2;
  var4 = ((var3 / 32768) * (var3 / 32768)) / 128;
  var5 = var3 - ((var4 * int32_t{c.h1}) / 16);
  var5 = var5 < 0 ? 0 : var5;
  var5 = var5 > 419430400 ? 419430400 : var5;
  return static_cast<float>(static_cast<uint32_t>(var5 / 4096)) / 1024.0f;
}

struct Measurement {
  float temperatureC = NAN;
  float humidityPct = NAN;
  float pressurePa = NAN;
};

// A channel still at its reset value was not measured: NaN, as is every
// channel when temperature (which the others are compensated with) is.
inline Measurement compensate(const Calibration& c, const Raw& raw) {
  Measurement out;
  if (raw.temperature == kSkipped20Bit) {
    return out;
  }
  const int32_t tFine = temperatureFine(c, raw.temperature);
  out.temperatureC = temperatureC(tFine);
  if (raw.pressure != kSkipped20Bit) {
    out.pressurePa = pressurePa(c, tFine, raw.pressure);
  }
  if (raw.humidity != kSkipped16Bit) {
    out.humidityPct = humidityPct(c, tFine, raw.humidity);
  }
  return out;
}

// The sensor's rated 300-1100 hPa. A failed read used to decode as a few hPa,
// which passed the old "> 0" check.
inline bool validPressurePa(float value) {
  return isfinite(value) && value >= 30000.0f && value <= 110000.0f;
}

}  // namespace Bme280
