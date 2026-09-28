// SPDX-License-Identifier: GPL-3.0-only
#include "CalibrationPolicy.h"

#include <math.h>

#include "../model/Config.h"
#include "../model/Reading.h"

namespace {

// Saturation vapour pressure over water, Magnus form (Sonntag 1990
// coefficients), hPa. Only ratios are used, so the scale factor cancels.
float saturationVapourPressure(float temperatureC) {
  return 6.112f * expf(17.62f * temperatureC / (243.12f + temperatureC));
}

}  // namespace

void applyCalibration(Reading& reading, const Config& config) {
  if (reading.valid & VALID_NOISE) {
    reading.noiseDb += config.noiseOffsetDb;
  }
  const bool bothValid = (reading.valid & VALID_TEMPERATURE) &&
                         (reading.valid & VALID_HUMIDITY);
  if (bothValid && config.tempOffsetC != 0.0f) {
    // The temperature offset mostly corrects self-heating: the BME280 sits in
    // air warmer than the room, where the same moisture reads as lower RH.
    // Re-express RH at the corrected temperature, holding the vapour
    // pressure constant; the humidity offset then covers only sensor error.
    reading.humidityPct *=
        saturationVapourPressure(reading.temperatureC) /
        saturationVapourPressure(reading.temperatureC + config.tempOffsetC);
  }
  if (reading.valid & VALID_TEMPERATURE) {
    reading.temperatureC += config.tempOffsetC;
  }
  if (reading.valid & VALID_HUMIDITY) {
    reading.humidityPct += config.humidityOffsetRh;
    if (reading.humidityPct < 0.0f) {
      reading.humidityPct = 0.0f;
    } else if (reading.humidityPct > 100.0f) {
      reading.humidityPct = 100.0f;
    }
  }
}
