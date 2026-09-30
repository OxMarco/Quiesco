// SPDX-License-Identifier: GPL-3.0-only
#include <cmath>
#include <vector>

#include "doctest.h"

#include "dsp/AcousticMetrics.h"
#include "model/Config.h"
#include "model/FaultStatus.h"
#include "model/Reading.h"
#include "services/SampleRecord.h"
#include "services/Scheduler.h"
#include "ui/ComfortEvaluation.h"
#include "ui/UiModel.h"

namespace {




TEST_CASE("config validation") {
  const Config defaults = defaultConfig();
  CHECK(isValidConfig(defaults));
  CHECK(defaults.displayScreen == DISPLAY_SCREEN_BENTO);
  CHECK(isValidMeasurementInterval(60));
  CHECK(isValidMeasurementInterval(300));
  CHECK(isValidMeasurementInterval(600));
  CHECK(isValidMeasurementInterval(1800));
  CHECK(!isValidMeasurementInterval(0));
  CHECK(!isValidMeasurementInterval(120));
  CHECK(!isValidMeasurementInterval(3600));

  CHECK(isValidDeviceName("Quiesco"));
  CHECK(isValidDeviceName("0123456789abcdef"));  // 16 bytes, the max
  CHECK(!isValidDeviceName(""));
  CHECK(!isValidDeviceName("0123456789abcdefg"));  // 17 bytes
  CHECK(!isValidDeviceName("bad\tname"));          // control byte

  CHECK(isValidCalibrationOffsets(-24.0f, 8.0f, -20.0f));
  CHECK(!isValidCalibrationOffsets(-25.0f, 0.0f, 0.0f));
  CHECK(!isValidCalibrationOffsets(0.0f, 8.5f, 0.0f));
  CHECK(!isValidCalibrationOffsets(0.0f, 0.0f, 20.5f));
  Config badName = defaults;
  badName.deviceName[0] = '\0';
  CHECK(!isValidConfig(badName));
  Config badOffset = defaults;
  badOffset.tempOffsetC = 99.0f;
  CHECK(!isValidConfig(badOffset));

  Config invalid = defaults;
  invalid.version++;
  CHECK(!isValidConfig(invalid));
  invalid = defaults;
  invalid.fullRefreshEveryCycles = 0;
  CHECK(!isValidConfig(invalid));

  CHECK(isValidDisplayScreen(DISPLAY_SCREEN_FACE));
  CHECK(isValidDisplayScreen(DISPLAY_SCREEN_LEDGER));
  CHECK(isValidDisplayScreen(DISPLAY_SCREEN_BENTO));
  CHECK(!isValidDisplayScreen(3));
  invalid = defaults;
  invalid.displayScreen = 3;
  CHECK(!isValidConfig(invalid));
}

TEST_CASE("immediate boot and deadline progression") {
  Scheduler scheduler;
  scheduler.begin(1250, 60);
  CHECK(scheduler.claimDue(1250));
  CHECK_EQ(uint64_t{61250}, scheduler.nextDeadlineMs());
  CHECK(!scheduler.claimDue(61249));
  CHECK(scheduler.claimDue(61250));
  CHECK_EQ(uint64_t{121250}, scheduler.nextDeadlineMs());
}

TEST_CASE("missed deadlines coalesce without drift") {
  Scheduler scheduler;
  scheduler.begin(1000, 60);
  CHECK(scheduler.claimDue(1000));
  CHECK(scheduler.claimDue(190000));
  CHECK_EQ(uint64_t{241000}, scheduler.nextDeadlineMs());
  CHECK(!scheduler.claimDue(190000));
}

TEST_CASE("interval change starts from now") {
  Scheduler scheduler;
  scheduler.begin(0, 300);
  CHECK(scheduler.claimDue(0));
  scheduler.changeInterval(45000, 60);
  CHECK_EQ(uint64_t{105000}, scheduler.nextDeadlineMs());
  CHECK(!scheduler.claimDue(104999));
  CHECK(scheduler.claimDue(105000));
  CHECK_EQ(uint32_t{60}, scheduler.intervalSeconds());
}

TEST_CASE("deadline crosses millis() rollover") {
  const uint64_t beforeRollover = (uint64_t{1} << 32) - 1000;
  Scheduler scheduler;
  scheduler.begin(beforeRollover, 60);
  CHECK(scheduler.claimDue(beforeRollover));
  CHECK_EQ(beforeRollover + 60000, scheduler.nextDeadlineMs());
  CHECK(scheduler.nextDeadlineMs() > (uint64_t{1} << 32));
}

TEST_CASE("fault counters recover and saturate") {
  DeviceFault fault;
  recordDeviceResult(fault, false, false, false);
  CHECK_EQ(uint16_t{1}, fault.consecutiveFailures);
  CHECK(!fault.present);

  recordDeviceResult(fault, true, true, false);
  CHECK_EQ(uint16_t{2}, fault.consecutiveFailures);
  CHECK(fault.present);
  CHECK(fault.timedOut);

  recordDeviceResult(fault, true, false, true);
  CHECK_EQ(uint16_t{0}, fault.consecutiveFailures);
  CHECK(!fault.timedOut);

  fault.consecutiveFailures = UINT16_MAX;
  recordDeviceResult(fault, true, false, false);
  CHECK_EQ(UINT16_MAX, fault.consecutiveFailures);
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

TEST_CASE("comfort bands") {
  CHECK(Severity::kOk == bandSeverity(UiMetric::kCo2, 800.0f));
  CHECK(Severity::kWarn == bandSeverity(UiMetric::kCo2, 801.0f));
  CHECK(Severity::kWarn == bandSeverity(UiMetric::kCo2, 1200.0f));
  CHECK(Severity::kBad == bandSeverity(UiMetric::kCo2, 1201.0f));

  CHECK(Severity::kBad == bandSeverity(UiMetric::kTemperature, 17.0f));
  CHECK(Severity::kWarn == bandSeverity(UiMetric::kTemperature, 19.0f));
  CHECK(Severity::kOk == bandSeverity(UiMetric::kTemperature, 23.0f));
  CHECK(Severity::kWarn == bandSeverity(UiMetric::kTemperature, 27.0f));
  CHECK(Severity::kBad == bandSeverity(UiMetric::kTemperature, 29.0f));

  CHECK(Severity::kOk == bandSeverity(UiMetric::kNoise, 55.0f));
  CHECK(Severity::kWarn == bandSeverity(UiMetric::kNoise, 56.0f));
  CHECK(Severity::kBad == bandSeverity(UiMetric::kNoise, 71.0f));

  // Light never leaves comfort: it must not drive the face.
  CHECK(Severity::kOk == bandSeverity(UiMetric::kLight, 100000.0f));
}

TEST_CASE("worst metric selection and priority") {
  Reading reading = comfortableReading();
  UiModel model = buildUiModel(reading, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false);
  CHECK(model.faceSeverity == Severity::kOk);
  CHECK(model.screen == ScreenId::kFace);

  reading.co2Ppm = 1512.0f;
  model = buildUiModel(reading, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false);
  CHECK(model.faceSeverity == Severity::kBad);
  CHECK(model.worstMetric == UiMetric::kCo2);
  CHECK(model.worstAbove);

  // Equal severity: CO2 outranks noise (enum order is the tie-break).
  reading.co2Ppm = 900.0f;
  reading.noiseDb = 60.0f;
  model = buildUiModel(reading, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false);
  CHECK(model.faceSeverity == Severity::kWarn);
  CHECK(model.worstMetric == UiMetric::kCo2);

  // A worse metric wins regardless of order; an invalid one is ignored.
  reading.noiseDb = 80.0f;
  model = buildUiModel(reading, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false);
  CHECK(model.worstMetric == UiMetric::kNoise);
  reading.valid &= ~VALID_NOISE;
  model = buildUiModel(reading, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false);
  CHECK(model.worstMetric == UiMetric::kCo2);
}

TEST_CASE("screen selection and battery override") {
  const Reading reading = comfortableReading();
  CHECK(buildUiModel(reading, DISPLAY_SCREEN_LEDGER, TEMPERATURE_UNIT_CELSIUS, false).screen ==
        ScreenId::kLedger);
  CHECK(buildUiModel(reading, DISPLAY_SCREEN_BENTO, TEMPERATURE_UNIT_CELSIUS, false).screen ==
        ScreenId::kBento);
  CHECK(buildUiModel(reading, DISPLAY_SCREEN_BENTO, TEMPERATURE_UNIT_CELSIUS, true).screen ==
        ScreenId::kBattery);

  Reading lowBattery = reading;
  lowBattery.batteryV = 3.4f;
  CHECK(buildUiModel(lowBattery, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false).screen ==
        ScreenId::kBattery);
  // Unknown battery voltage must not trap the UI on the battery screen.
  lowBattery.valid &= ~VALID_BATTERY;
  CHECK(buildUiModel(lowBattery, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false).screen ==
        ScreenId::kFace);
}

// °F changes only the digits drawn: severity and the face's verdict are still
// judged on the rounded °C value, and switching units forces a redraw.
TEST_CASE("fahrenheit display") {
  Reading reading = comfortableReading();
  reading.temperatureC = 29.4f;  // 85 °F, above the 28 °C bad limit
  const uint8_t t = static_cast<uint8_t>(UiMetric::kTemperature);
  const UiModel c = buildUiModel(reading, DISPLAY_SCREEN_FACE,
                                 TEMPERATURE_UNIT_CELSIUS, false);
  const UiModel f = buildUiModel(reading, DISPLAY_SCREEN_FACE,
                                 TEMPERATURE_UNIT_FAHRENHEIT, false);
  CHECK_EQ(int32_t{29}, c.values[t]);
  CHECK_EQ(int32_t{85}, f.values[t]);
  CHECK((f.fahrenheit && !c.fahrenheit));
  CHECK(f.severities[t] == c.severities[t]);
  CHECK(f.faceSeverity == Severity::kBad);
  CHECK((f.worstMetric == UiMetric::kTemperature && f.worstAbove));
  CHECK(!hasSameRenderedContent(c, f));

  // Other metrics are untouched.
  const uint8_t h = static_cast<uint8_t>(UiMetric::kHumidity);
  CHECK_EQ(c.values[h], f.values[h]);
}

// Covers hasSameRenderedContent, the comparison App actually gates panel
// refreshes on. It is deliberately per-screen: a metric that changed but is
// not drawn on the current screen must not cost a refresh.
TEST_CASE("UiModel skip comparison") {
  const Reading reading = comfortableReading();
  const UiModel a = buildUiModel(reading, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false);
  CHECK(hasSameRenderedContent(
      a, buildUiModel(reading, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false)));

  // A resting face draws no values, so a comfortable metric moving (even past
  // a rounding step) changes nothing on screen.
  Reading jitter = reading;
  jitter.temperatureC += 1.2f;
  CHECK(hasSameRenderedContent(
      a, buildUiModel(jitter, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false)));

  // Leaving the comfort band does change the face, and the callout it draws
  // tracks the offending value.
  Reading hot = reading;
  hot.co2Ppm = 1512.0f;
  const UiModel bad = buildUiModel(hot, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false);
  CHECK(!hasSameRenderedContent(a, bad));
  hot.co2Ppm = 1610.0f;
  CHECK(
      !hasSameRenderedContent(bad, buildUiModel(hot, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS,
                                          false)));

  // The ledger draws every metric, so the same sub-band move does matter there.
  const UiModel ledger = buildUiModel(reading, DISPLAY_SCREEN_LEDGER, TEMPERATURE_UNIT_CELSIUS, false);
  CHECK(!hasSameRenderedContent(
      ledger, buildUiModel(jitter, DISPLAY_SCREEN_LEDGER, TEMPERATURE_UNIT_CELSIUS, false)));

  // Charging state is only drawn on the battery screen.
  Reading low = reading;
  low.batteryV = 3.4f;
  const UiModel battery = buildUiModel(low, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, false);
  CHECK(battery.screen == ScreenId::kBattery);
  CHECK(!hasSameRenderedContent(
      battery, buildUiModel(low, DISPLAY_SCREEN_FACE, TEMPERATURE_UNIT_CELSIUS, true)));

  // The pairing code redraws only when the code changes, and never compares
  // equal to a reading screen.
  const UiModel pairing = buildPairingModel(482913);
  CHECK(pairing.screen == ScreenId::kPairing);
  CHECK(hasSameRenderedContent(pairing, buildPairingModel(482913)));
  CHECK(!hasSameRenderedContent(pairing, buildPairingModel(482914)));
  CHECK(!hasSameRenderedContent(pairing, battery));
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

TEST_CASE("A-weighting") {
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
    CAPTURE(point.hz);
    CAPTURE(point.expectedDb);
    CAPTURE(got);
    CHECK(std::fabs(got - point.expectedDb) <= point.toleranceDb);
  }
}

TEST_CASE("acoustic metrics") {
  constexpr uint16_t kCount = 1024;
  AcousticMetrics metrics;
  metrics.reset();
  CHECK(!metrics.valid());  // empty window is never audio

  // Pure DC reads as silence: the mean is removed per buffer.
  const auto dcOnly = squareWave(0, 1000, kCount);
  metrics.addBuffer(dcOnly.data(), kCount);
  CHECK(metrics.rms() < 1.0f);
  CHECK_EQ(int32_t{1000}, metrics.dcMean());
  CHECK(!metrics.valid());

  // A +/-1000 square is RMS 1000 and peak 1000, DC offset notwithstanding.
  metrics.reset();
  const auto square = squareWave(1000, 200, kCount);
  metrics.addBuffer(square.data(), kCount);
  CHECK(std::fabs(metrics.rms() - 1000.0f) < 1.0f);
  CHECK_EQ(int32_t{1000}, metrics.peak());
  CHECK_EQ(uint32_t{0}, metrics.clipped());
  CHECK(metrics.valid());

  // The stopped-clock signature -- samples pinned at the rail -- is invalid
  // even though its RMS is huge.
  metrics.reset();
  const auto pinned = squareWave(32200, 0, kCount);
  metrics.addBuffer(pinned.data(), kCount);
  CHECK_EQ(uint32_t{kCount}, metrics.clipped());
  CHECK(!metrics.valid());

  // Multi-buffer accumulation is DC-shift independent per buffer.
  metrics.reset();
  const auto shifted = squareWave(1000, -300, kCount);
  metrics.addBuffer(square.data(), kCount);
  metrics.addBuffer(shifted.data(), kCount);
  CHECK_EQ(uint16_t{2}, metrics.buffers());
  CHECK(std::fabs(metrics.rms() - 1000.0f) < 1.0f);

  // Half-scale square at +8 dB PDM gain: -6 dBFS - 8 + 120 ~ 106 dB SPL.
  metrics.reset();
  const auto halfScale = squareWave(16384, 0, kCount);
  metrics.addBuffer(halfScale.data(), kCount);
  CHECK(std::fabs(metrics.dbSpl(8.0f) - 106.0f) < 0.1f);
  // Quieter signal, lower level.
  AcousticMetrics quiet;
  quiet.reset();
  quiet.addBuffer(square.data(), kCount);
  CHECK(quiet.dbSpl(8.0f) < metrics.dbSpl(8.0f));
}

TEST_CASE("sample record codec") {
  SampleRecord record = {};
  record.sequence = 42;
  record.validFlags = 0x7F;
  record.monotonicMs = 1234567;
  record.temperatureC = 23.5f;
  record.co2Ppm = 640.0f;
  sealSampleRecord(record);
  CHECK(isValidSampleRecord(record));

  // Any corruption must fail the CRC; sequence sentinels are invalid even
  // with a matching CRC (0xFFFFFFFF is erased flash, 0 a torn write).
  SampleRecord corrupted = record;
  corrupted.co2Ppm = 641.0f;
  CHECK(!isValidSampleRecord(corrupted));
  corrupted = record;
  corrupted.sequence = 0;
  sealSampleRecord(corrupted);
  CHECK(!isValidSampleRecord(corrupted));
  corrupted.sequence = SampleRecord::kInvalidSequenceErased;
  sealSampleRecord(corrupted);
  CHECK(!isValidSampleRecord(corrupted));

  // An erased slot (all 0xFF) must never scan as a record.
  SampleRecord erased;
  unsigned char* bytes = reinterpret_cast<unsigned char*>(&erased);
  for (unsigned i = 0; i < sizeof erased; i++) bytes[i] = 0xFF;
  CHECK(!isValidSampleRecord(erased));

  // CRC-16/CCITT reference value ("123456789" -> 0x29B1).
  CHECK_EQ(uint16_t{0x29B1},
           crc16Ccitt(reinterpret_cast<const uint8_t*>("123456789"), 9));
}

TEST_CASE("battery percent from volts") {
  CHECK_EQ(0u, unsigned{batteryPercentFromVolts(3.0f)});
  CHECK_EQ(0u, unsigned{batteryPercentFromVolts(3.3f)});
  CHECK_EQ(5u, unsigned{batteryPercentFromVolts(3.5f)});
  CHECK_EQ(50u, unsigned{batteryPercentFromVolts(3.8f)});
  CHECK_EQ(100u, unsigned{batteryPercentFromVolts(4.2f)});
  CHECK_EQ(100u, unsigned{batteryPercentFromVolts(4.5f)});
  CHECK(batteryPercentFromVolts(3.75f) < batteryPercentFromVolts(3.85f));
}

}  // namespace

