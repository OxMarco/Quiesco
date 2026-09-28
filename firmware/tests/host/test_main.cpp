// SPDX-License-Identifier: GPL-3.0-only
#include <cstdlib>
#include <iostream>

#include <cmath>
#include <vector>

#include "dsp/AcousticMetrics.h"
#include "model/Config.h"
#include "model/FaultStatus.h"
#include "model/Reading.h"
#include "services/SampleRecord.h"
#include "services/Scheduler.h"
#include "ui/ComfortEvaluation.h"
#include "ui/UiModel.h"

namespace {

int failures = 0;

#define EXPECT_TRUE(expression)                                                \
  do {                                                                         \
    if (!(expression)) {                                                       \
      std::cerr << __FILE__ << ':' << __LINE__ << ": expected " #expression  \
                << '\n';                                                       \
      ++failures;                                                              \
    }                                                                          \
  } while (false)

#define EXPECT_EQ(expected, actual)                                            \
  do {                                                                         \
    const auto expectedValue = (expected);                                     \
    const auto actualValue = (actual);                                         \
    if (expectedValue != actualValue) {                                        \
      std::cerr << __FILE__ << ':' << __LINE__ << ": expected "               \
                << expectedValue << ", got " << actualValue << '\n';           \
      ++failures;                                                              \
    }                                                                          \
  } while (false)

void testConfigValidation() {
  const Config defaults = defaultConfig();
  EXPECT_TRUE(isValidConfig(defaults));
  EXPECT_TRUE(defaults.displayScreen == DISPLAY_SCREEN_BENTO);
  EXPECT_TRUE(isValidMeasurementInterval(60));
  EXPECT_TRUE(isValidMeasurementInterval(300));
  EXPECT_TRUE(isValidMeasurementInterval(600));
  EXPECT_TRUE(isValidMeasurementInterval(1800));
  EXPECT_TRUE(!isValidMeasurementInterval(0));
  EXPECT_TRUE(!isValidMeasurementInterval(120));
  EXPECT_TRUE(!isValidMeasurementInterval(3600));

  EXPECT_TRUE(isValidDeviceName("Quiesco"));
  EXPECT_TRUE(isValidDeviceName("0123456789abcdef"));  // 16 bytes, the max
  EXPECT_TRUE(!isValidDeviceName(""));
  EXPECT_TRUE(!isValidDeviceName("0123456789abcdefg"));  // 17 bytes
  EXPECT_TRUE(!isValidDeviceName("bad\tname"));          // control byte

  EXPECT_TRUE(isValidCalibrationOffsets(-24.0f, 8.0f, -20.0f));
  EXPECT_TRUE(!isValidCalibrationOffsets(-25.0f, 0.0f, 0.0f));
  EXPECT_TRUE(!isValidCalibrationOffsets(0.0f, 8.5f, 0.0f));
  EXPECT_TRUE(!isValidCalibrationOffsets(0.0f, 0.0f, 20.5f));
  Config badName = defaults;
  badName.deviceName[0] = '\0';
  EXPECT_TRUE(!isValidConfig(badName));
  Config badOffset = defaults;
  badOffset.tempOffsetC = 99.0f;
  EXPECT_TRUE(!isValidConfig(badOffset));

  Config invalid = defaults;
  invalid.version++;
  EXPECT_TRUE(!isValidConfig(invalid));
  invalid = defaults;
  invalid.fullRefreshEveryCycles = 0;
  EXPECT_TRUE(!isValidConfig(invalid));

  EXPECT_TRUE(isValidDisplayScreen(DISPLAY_SCREEN_FACE));
  EXPECT_TRUE(isValidDisplayScreen(DISPLAY_SCREEN_LEDGER));
  EXPECT_TRUE(isValidDisplayScreen(DISPLAY_SCREEN_BENTO));
  EXPECT_TRUE(!isValidDisplayScreen(3));
  invalid = defaults;
  invalid.displayScreen = 3;
  EXPECT_TRUE(!isValidConfig(invalid));
}

void testImmediateBootAndDeadlineProgression() {
  Scheduler scheduler;
  scheduler.begin(1250, 60);
  EXPECT_TRUE(scheduler.claimDue(1250));
  EXPECT_EQ(uint64_t{61250}, scheduler.nextDeadlineMs());
  EXPECT_TRUE(!scheduler.claimDue(61249));
  EXPECT_TRUE(scheduler.claimDue(61250));
  EXPECT_EQ(uint64_t{121250}, scheduler.nextDeadlineMs());
}

void testMissedDeadlinesCoalesceWithoutDrift() {
  Scheduler scheduler;
  scheduler.begin(1000, 60);
  EXPECT_TRUE(scheduler.claimDue(1000));
  EXPECT_TRUE(scheduler.claimDue(190000));
  EXPECT_EQ(uint64_t{241000}, scheduler.nextDeadlineMs());
  EXPECT_TRUE(!scheduler.claimDue(190000));
}

void testIntervalChangeStartsFromNow() {
  Scheduler scheduler;
  scheduler.begin(0, 300);
  EXPECT_TRUE(scheduler.claimDue(0));
  scheduler.changeInterval(45000, 60);
  EXPECT_EQ(uint64_t{105000}, scheduler.nextDeadlineMs());
  EXPECT_TRUE(!scheduler.claimDue(104999));
  EXPECT_TRUE(scheduler.claimDue(105000));
  EXPECT_EQ(uint32_t{60}, scheduler.intervalSeconds());
}

void testDeadlineCrossesMillisRollover() {
  const uint64_t beforeRollover = (uint64_t{1} << 32) - 1000;
  Scheduler scheduler;
  scheduler.begin(beforeRollover, 60);
  EXPECT_TRUE(scheduler.claimDue(beforeRollover));
  EXPECT_EQ(beforeRollover + 60000, scheduler.nextDeadlineMs());
  EXPECT_TRUE(scheduler.nextDeadlineMs() > (uint64_t{1} << 32));
}

void testFaultCountersRecoverAndSaturate() {
  DeviceFault fault;
  recordDeviceResult(fault, false, false, false);
  EXPECT_EQ(uint16_t{1}, fault.consecutiveFailures);
  EXPECT_TRUE(!fault.present);

  recordDeviceResult(fault, true, true, false);
  EXPECT_EQ(uint16_t{2}, fault.consecutiveFailures);
  EXPECT_TRUE(fault.present);
  EXPECT_TRUE(fault.timedOut);

  recordDeviceResult(fault, true, false, true);
  EXPECT_EQ(uint16_t{0}, fault.consecutiveFailures);
  EXPECT_TRUE(!fault.timedOut);

  fault.consecutiveFailures = UINT16_MAX;
  recordDeviceResult(fault, true, false, false);
  EXPECT_EQ(UINT16_MAX, fault.consecutiveFailures);
}

Reading comfortableReading() {
  Reading reading;
  reading.temperatureC = 23.0f;
  reading.humidityPct = 45.0f;
  reading.co2Ppm = 640.0f;
  reading.lux = 210.0f;
  reading.noiseDb = 38.0f;
  reading.batteryV = 3.9f;
  reading.valid = VALID_TEMPERATURE | VALID_HUMIDITY | VALID_CO2 |
                  VALID_LIGHT | VALID_NOISE | VALID_BATTERY;
  return reading;
}

void testComfortBands() {
  EXPECT_TRUE(Severity::kOk == bandSeverity(UiMetric::kCo2, 800.0f));
  EXPECT_TRUE(Severity::kWarn == bandSeverity(UiMetric::kCo2, 801.0f));
  EXPECT_TRUE(Severity::kWarn == bandSeverity(UiMetric::kCo2, 1200.0f));
  EXPECT_TRUE(Severity::kBad == bandSeverity(UiMetric::kCo2, 1201.0f));

  EXPECT_TRUE(Severity::kBad == bandSeverity(UiMetric::kTemperature, 17.0f));
  EXPECT_TRUE(Severity::kWarn == bandSeverity(UiMetric::kTemperature, 19.0f));
  EXPECT_TRUE(Severity::kOk == bandSeverity(UiMetric::kTemperature, 23.0f));
  EXPECT_TRUE(Severity::kWarn == bandSeverity(UiMetric::kTemperature, 27.0f));
  EXPECT_TRUE(Severity::kBad == bandSeverity(UiMetric::kTemperature, 29.0f));

  EXPECT_TRUE(Severity::kOk == bandSeverity(UiMetric::kNoise, 55.0f));
  EXPECT_TRUE(Severity::kWarn == bandSeverity(UiMetric::kNoise, 56.0f));
  EXPECT_TRUE(Severity::kBad == bandSeverity(UiMetric::kNoise, 71.0f));

  // Light never leaves comfort: it must not drive the face.
  EXPECT_TRUE(Severity::kOk == bandSeverity(UiMetric::kLight, 100000.0f));
}

void testWorstMetricSelectionAndPriority() {
  Reading reading = comfortableReading();
  UiModel model = buildUiModel(reading, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false);
  EXPECT_TRUE(model.faceSeverity == Severity::kOk);
  EXPECT_TRUE(model.screen == ScreenId::kFace);

  reading.co2Ppm = 1512.0f;
  model = buildUiModel(reading, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false);
  EXPECT_TRUE(model.faceSeverity == Severity::kBad);
  EXPECT_TRUE(model.worstMetric == UiMetric::kCo2);
  EXPECT_TRUE(model.worstAbove);

  // Equal severity: CO2 outranks noise (enum order is the tie-break).
  reading.co2Ppm = 900.0f;
  reading.noiseDb = 60.0f;
  model = buildUiModel(reading, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false);
  EXPECT_TRUE(model.faceSeverity == Severity::kWarn);
  EXPECT_TRUE(model.worstMetric == UiMetric::kCo2);

  // A worse metric wins regardless of order; an invalid one is ignored.
  reading.noiseDb = 80.0f;
  model = buildUiModel(reading, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false);
  EXPECT_TRUE(model.worstMetric == UiMetric::kNoise);
  reading.valid &= ~VALID_NOISE;
  model = buildUiModel(reading, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false);
  EXPECT_TRUE(model.worstMetric == UiMetric::kCo2);
}

void testScreenSelectionAndBatteryOverride() {
  const Reading reading = comfortableReading();
  EXPECT_TRUE(buildUiModel(reading, DISPLAY_SCREEN_LEDGER, TEMPERATURE_UNIT_CELSIUS, false).screen ==
              ScreenId::kLedger);
  EXPECT_TRUE(buildUiModel(reading, DISPLAY_SCREEN_BENTO, TEMPERATURE_UNIT_CELSIUS, false).screen ==
              ScreenId::kBento);
  EXPECT_TRUE(buildUiModel(reading, DISPLAY_SCREEN_BENTO, TEMPERATURE_UNIT_CELSIUS, true).screen ==
              ScreenId::kBattery);

  Reading lowBattery = reading;
  lowBattery.batteryV = 3.4f;
  EXPECT_TRUE(buildUiModel(lowBattery, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false).screen ==
              ScreenId::kBattery);
  // Unknown battery voltage must not trap the UI on the battery screen.
  lowBattery.valid &= ~VALID_BATTERY;
  EXPECT_TRUE(buildUiModel(lowBattery, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false).screen ==
              ScreenId::kFace);
}

// °F changes only the digits drawn: severity and the face's verdict are still
// judged on the rounded °C value, and switching units forces a redraw.
void testFahrenheitDisplay() {
  Reading reading = comfortableReading();
  reading.temperatureC = 29.4f;  // 85 °F, above the 28 °C bad limit
  const uint8_t t = static_cast<uint8_t>(UiMetric::kTemperature);
  const UiModel c = buildUiModel(reading, DISPLAY_SCREEN_FACE,
                                 TEMPERATURE_UNIT_CELSIUS, false);
  const UiModel f = buildUiModel(reading, DISPLAY_SCREEN_FACE,
                                 TEMPERATURE_UNIT_FAHRENHEIT, false);
  EXPECT_EQ(int32_t{29}, c.values[t]);
  EXPECT_EQ(int32_t{85}, f.values[t]);
  EXPECT_TRUE(f.fahrenheit && !c.fahrenheit);
  EXPECT_TRUE(f.severities[t] == c.severities[t]);
  EXPECT_TRUE(f.faceSeverity == Severity::kBad);
  EXPECT_TRUE(f.worstMetric == UiMetric::kTemperature && f.worstAbove);
  EXPECT_TRUE(!hasSameRenderedContent(c, f));

  // Other metrics are untouched.
  const uint8_t h = static_cast<uint8_t>(UiMetric::kHumidity);
  EXPECT_EQ(c.values[h], f.values[h]);
}

// Covers hasSameRenderedContent, the comparison App actually gates panel
// refreshes on. It is deliberately per-screen: a metric that changed but is
// not drawn on the current screen must not cost a refresh.
void testUiModelSkipComparison() {
  const Reading reading = comfortableReading();
  const UiModel a = buildUiModel(reading, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false);
  EXPECT_TRUE(hasSameRenderedContent(
      a, buildUiModel(reading, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false)));

  // A resting face draws no values, so a comfortable metric moving (even past
  // a rounding step) changes nothing on screen.
  Reading jitter = reading;
  jitter.temperatureC += 1.2f;
  EXPECT_TRUE(hasSameRenderedContent(
      a, buildUiModel(jitter, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false)));

  // Leaving the comfort band does change the face, and the callout it draws
  // tracks the offending value.
  Reading hot = reading;
  hot.co2Ppm = 1512.0f;
  const UiModel bad = buildUiModel(hot, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false);
  EXPECT_TRUE(!hasSameRenderedContent(a, bad));
  hot.co2Ppm = 1610.0f;
  EXPECT_TRUE(
      !hasSameRenderedContent(bad, buildUiModel(hot, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS,
                                                false)));

  // The ledger draws every metric, so the same sub-band move does matter there.
  const UiModel ledger = buildUiModel(reading, DISPLAY_SCREEN_LEDGER, TEMPERATURE_UNIT_CELSIUS, false);
  EXPECT_TRUE(!hasSameRenderedContent(
      ledger, buildUiModel(jitter, DISPLAY_SCREEN_LEDGER, TEMPERATURE_UNIT_CELSIUS, false)));

  // Charging state is only drawn on the battery screen.
  Reading low = reading;
  low.batteryV = 3.4f;
  const UiModel battery = buildUiModel(low, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false);
  EXPECT_TRUE(battery.screen == ScreenId::kBattery);
  EXPECT_TRUE(!hasSameRenderedContent(
      battery, buildUiModel(low, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, true)));

  // The pairing code redraws only when the code changes, and never compares
  // equal to a reading screen.
  const UiModel pairing = buildPairingModel(482913);
  EXPECT_TRUE(pairing.screen == ScreenId::kPairing);
  EXPECT_TRUE(hasSameRenderedContent(pairing, buildPairingModel(482913)));
  EXPECT_TRUE(!hasSameRenderedContent(pairing, buildPairingModel(482914)));
  EXPECT_TRUE(!hasSameRenderedContent(pairing, battery));
}

std::vector<int16_t> squareWave(int16_t amplitude, int16_t dcOffset,
                                uint16_t count) {
  std::vector<int16_t> samples(count);
  for (uint16_t i = 0; i < count; ++i) {
    samples[i] =
        static_cast<int16_t>(dcOffset + (i % 2 == 0 ? amplitude : -amplitude));
  }
  return samples;
}

// A-weighted level of a pure tone relative to its unweighted level, dB. The
// mic's DC offset rides along, as it does on hardware.
float aWeightingGainDb(float frequencyHz) {
  constexpr uint16_t kCount = 1024;
  constexpr float kPi = 3.14159265358979f;
  AcousticMetrics metrics;
  metrics.reset();
  std::vector<int16_t> buffer(kCount);
  uint32_t n = 0;
  for (uint8_t b = 0; b < 12; ++b) {
    for (uint16_t i = 0; i < kCount; ++i, ++n) {
      const float phase = 2.0f * kPi * frequencyHz *
                          static_cast<float>(n) / AcousticMetrics::kSampleRateHz;
      buffer[i] = static_cast<int16_t>(800.0f + 10000.0f * std::sin(phase));
    }
    metrics.addBuffer(buffer.data(), kCount);
  }
  return 20.0f * std::log10(metrics.weightedRms() / (10000.0f / std::sqrt(2.0f)));
}

void testAWeighting() {
  // IEC 61672 A-weighting, dB: the filter must hold these within 0.3 dB
  // (tones near the band edges within 0.5 dB).
  const struct {
    float hz;
    float expectedDb;
    float toleranceDb;
  } points[] = {
      {31.5f, -39.5f, 0.5f}, {63.0f, -26.2f, 0.3f}, {100.0f, -19.1f, 0.3f},
      {250.0f, -8.6f, 0.3f}, {500.0f, -3.2f, 0.3f}, {1000.0f, 0.0f, 0.3f},
      {2000.0f, 1.2f, 0.3f}, {4000.0f, 1.0f, 0.3f}, {6300.0f, -0.1f, 0.5f},
  };
  for (const auto& point : points) {
    const float got = aWeightingGainDb(point.hz);
    if (std::fabs(got - point.expectedDb) > point.toleranceDb) {
      std::cerr << "A-weighting at " << point.hz << " Hz: expected "
                << point.expectedDb << " dB, got " << got << " dB\n";
      ++failures;
    }
  }
}

void testAcousticMetrics() {
  constexpr uint16_t kCount = 1024;
  AcousticMetrics metrics;
  metrics.reset();
  EXPECT_TRUE(!metrics.valid());  // empty window is never audio

  // Pure DC reads as silence: the mean is removed per buffer.
  const auto dcOnly = squareWave(0, 1000, kCount);
  metrics.addBuffer(dcOnly.data(), kCount);
  EXPECT_TRUE(metrics.rms() < 1.0f);
  EXPECT_EQ(int32_t{1000}, metrics.dcMean());
  EXPECT_TRUE(!metrics.valid());

  // A +/-1000 square is RMS 1000 and peak 1000, DC offset notwithstanding.
  metrics.reset();
  const auto square = squareWave(1000, 200, kCount);
  metrics.addBuffer(square.data(), kCount);
  EXPECT_TRUE(std::fabs(metrics.rms() - 1000.0f) < 1.0f);
  EXPECT_EQ(int32_t{1000}, metrics.peak());
  EXPECT_EQ(uint32_t{0}, metrics.clipped());
  EXPECT_TRUE(metrics.valid());

  // The stopped-clock signature -- samples pinned at the rail -- is invalid
  // even though its RMS is huge.
  metrics.reset();
  const auto pinned = squareWave(32200, 0, kCount);
  metrics.addBuffer(pinned.data(), kCount);
  EXPECT_EQ(uint32_t{kCount}, metrics.clipped());
  EXPECT_TRUE(!metrics.valid());

  // Multi-buffer accumulation is DC-shift independent per buffer.
  metrics.reset();
  const auto shifted = squareWave(1000, -300, kCount);
  metrics.addBuffer(square.data(), kCount);
  metrics.addBuffer(shifted.data(), kCount);
  EXPECT_EQ(uint16_t{2}, metrics.buffers());
  EXPECT_TRUE(std::fabs(metrics.rms() - 1000.0f) < 1.0f);

  // Half-scale square at +8 dB PDM gain: -6 dBFS - 8 + 120 ~ 106 dB SPL.
  metrics.reset();
  const auto halfScale = squareWave(16384, 0, kCount);
  metrics.addBuffer(halfScale.data(), kCount);
  EXPECT_TRUE(std::fabs(metrics.dbSpl(8.0f) - 106.0f) < 0.1f);
  // Quieter signal, lower level.
  AcousticMetrics quiet;
  quiet.reset();
  quiet.addBuffer(square.data(), kCount);
  EXPECT_TRUE(quiet.dbSpl(8.0f) < metrics.dbSpl(8.0f));
}

void testSampleRecordCodec() {
  SampleRecord record = {};
  record.sequence = 42;
  record.validFlags = 0x7F;
  record.monotonicMs = 1234567;
  record.temperatureC = 23.5f;
  record.co2Ppm = 640.0f;
  sealSampleRecord(record);
  EXPECT_TRUE(isValidSampleRecord(record));

  // Any corruption must fail the CRC; sequence sentinels are invalid even
  // with a matching CRC (0xFFFFFFFF is erased flash, 0 a torn write).
  SampleRecord corrupted = record;
  corrupted.co2Ppm = 641.0f;
  EXPECT_TRUE(!isValidSampleRecord(corrupted));
  corrupted = record;
  corrupted.sequence = 0;
  sealSampleRecord(corrupted);
  EXPECT_TRUE(!isValidSampleRecord(corrupted));
  corrupted.sequence = SampleRecord::kInvalidSequenceErased;
  sealSampleRecord(corrupted);
  EXPECT_TRUE(!isValidSampleRecord(corrupted));

  // An erased slot (all 0xFF) must never scan as a record.
  SampleRecord erased;
  unsigned char* bytes = reinterpret_cast<unsigned char*>(&erased);
  for (unsigned i = 0; i < sizeof erased; i++) bytes[i] = 0xFF;
  EXPECT_TRUE(!isValidSampleRecord(erased));

  // CRC-16/CCITT reference value ("123456789" -> 0x29B1).
  EXPECT_EQ(uint16_t{0x29B1},
            crc16Ccitt(reinterpret_cast<const uint8_t*>("123456789"), 9));
}

void testBatteryPercentFromVolts() {
  EXPECT_EQ(0u, unsigned{batteryPercentFromVolts(3.0f)});
  EXPECT_EQ(0u, unsigned{batteryPercentFromVolts(3.3f)});
  EXPECT_EQ(5u, unsigned{batteryPercentFromVolts(3.5f)});
  EXPECT_EQ(50u, unsigned{batteryPercentFromVolts(3.8f)});
  EXPECT_EQ(100u, unsigned{batteryPercentFromVolts(4.2f)});
  EXPECT_EQ(100u, unsigned{batteryPercentFromVolts(4.5f)});
  EXPECT_TRUE(batteryPercentFromVolts(3.75f) < batteryPercentFromVolts(3.85f));
}

}  // namespace

int main() {
  testConfigValidation();
  testImmediateBootAndDeadlineProgression();
  testMissedDeadlinesCoalesceWithoutDrift();
  testIntervalChangeStartsFromNow();
  testDeadlineCrossesMillisRollover();
  testFaultCountersRecoverAndSaturate();
  testComfortBands();
  testWorstMetricSelectionAndPriority();
  testScreenSelectionAndBatteryOverride();
  testUiModelSkipComparison();
  testFahrenheitDisplay();
  testAcousticMetrics();
  testAWeighting();
  testSampleRecordCodec();
  testBatteryPercentFromVolts();

  if (failures != 0) {
    std::cerr << failures << " host test(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "All Quiesco host tests passed\n";
  return EXIT_SUCCESS;
}
