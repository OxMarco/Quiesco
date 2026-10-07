// SPDX-License-Identifier: GPL-3.0-only
// Executed host tests for the flash-facing stores (over the in-memory
// W25Q64Flash sim) and the BLE wire codec.
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "doctest.h"

#include "W25Q64FlashSim.h"
#include "drivers/W25Q64Flash.h"
#include "model/Config.h"
#include "model/FaultStatus.h"
#include "model/Reading.h"
#include "model/SleepWindow.h"
#include "platform/HmacSha256.h"
#include "protocol/BleCodec.h"
#include "protocol/LittleEndian.h"
#include "services/CalibrationPolicy.h"
#include "services/ConfigRecord.h"
#include "services/ConfigStore.h"
#include "services/BondTable.h"
#include "services/SampleLog.h"
#include "services/UnitIdentity.h"
#include "services/WallClock.h"
#include "storage/FlashStorageLayout.h"

// Golden wire vectors (lowercase hex bytes); see testProtocolGoldenVectors.
#define GOLDEN_DEVICE_INFO \
  "06 00 ff 7f 00 00 01 02 03 02 f6 e5 d4 c3 b2 a1 " \
  "07 00 00 00"
#define GOLDEN_INTERVAL "2c 01 00 00"
#define GOLDEN_CORE_CONFIG "05 00 00 00 2c 01 00 00 0a 00 00 00 01 02 00 00"
#define GOLDEN_READING \
  "66 08 ad 11 cd 8b 01 00 64 02 0c 07 00 00 82 01 " \
  "48 0f 7f 00"
#define GOLDEN_STATUS \
  "6f 00 fd 00 00 03 00 00 00 00 00 00 d2 04 00 00 " \
  "01 03 e9 ff"
#define GOLDEN_CALIBRATION_STATE "00 b9 55 69 a4 01 db ff 01 00 00 00"
#define GOLDEN_DIAGNOSTICS "02 00 00 00 10 0e 00 00 00 00 00 00"
#define GOLDEN_EPOCH "00 a8 da 76 9b 01 00 00"
#define GOLDEN_OFFSETS "00 00 c0 bf 00 00 00 c0 00 00 40 40"
#define GOLDEN_SYNC_START "01 00 b1 04 00 00 00 00 f4 00"
#define GOLDEN_FRC "01 00 a4 01"
#define GOLDEN_RESET "01 01 c7 fa"
#define GOLDEN_ERASE_LOG "02 00 c7 fa a3 05 00 00"
#define GOLDEN_SLEEP_WINDOW "01 00 78 00 82 05 a4 01 ff ff ff ff"
#define GOLDEN_SLEEP_WINDOW_DEFAULT "00 00 00 00 82 05 a4 01 ff ff ff ff"
#define GOLDEN_WIRE_RECORD \
  "b1 04 00 00 7f 00 01 00 10 c7 55 69 80 ee 36 00 " \
  "00 00 00 00 00 00 ac 41 00 00 35 42 80 e6 c5 47 " \
  "00 00 19 44 66 66 34 43 66 66 1a 42 35 5e 7a 40 " \
  "07 00 00 00"
#define GOLDEN_DATA_HEADER "01 00 b1 04 00 00 01 00 01 00 00 06"
#define GOLDEN_FRAGMENT \
  "03 00 b1 04 00 00 01 00 01 00 00 35 b1 04 00 00 " \
  "7f 00 01 00"
#define GOLDEN_END "02 01 b2 04 00 00 00 00 00 00 00 ab"
#define GOLDEN_AUTH_STATE \
  "06 00 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 00 00"
#define GOLDEN_ENROL_KEY \
  "34 12 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
#define GOLDEN_ISSUED_KEY \
  "34 12 00 00 a0 a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab ac ad ae af"
#define GOLDEN_SETUP_PROVE \
  "02 00 34 12 05 45 13 4e 24 62 bb 4d cb 21 37 60 44 ae ca 9e"
#define GOLDEN_ENROL_WRITE "01 00"
#define GOLDEN_AUTH_PROVE \
  "02 00 34 12 f0 64 2a b3 02 06 0f 2f 61 13 6b 3f 1d ab 8a 1a"

namespace {




TEST_CASE("wall clock") {
  WallClock clock;
  CHECK(!clock.synced());
  CHECK_EQ(uint64_t{0}, clock.epochMsAt(123456));

  clock.sync(1752000000000ULL, 60000);
  CHECK(clock.synced());
  CHECK_EQ(1752000000000ULL, clock.epochMsAt(60000));
  CHECK_EQ(1752000005000ULL, clock.epochMsAt(65000));
  CHECK_EQ(1751999999000ULL, clock.epochMsAt(59000));  // pre-sync capture

  clock.sync(0, 70000);  // zero epoch cannot mark the clock synced
  CHECK(!clock.synced());
}

TEST_CASE("calibration policy") {
  Config config = defaultConfig();
  config.noiseOffsetDb = -3.0f;
  config.tempOffsetC = -1.5f;
  config.humidityOffsetRh = 10.0f;

  Reading reading;
  reading.noiseDb = 40.0f;
  reading.temperatureC = 24.0f;
  reading.humidityPct = 95.0f;
  reading.valid = VALID_NOISE | VALID_TEMPERATURE | VALID_HUMIDITY;
  applyCalibration(reading, config);
  CHECK(reading.noiseDb == 37.0f);
  CHECK(reading.temperatureC == 22.5f);
  CHECK(reading.humidityPct == 100.0f);  // clamped

  // Self-heating: the unit reads 27 C / 59 % beside a reference at
  // 26 C / 63 % (one field comparison). A -1 C offset alone must bring RH to
  // about 62.6 %, the same moisture at the corrected temperature.
  Config selfHeating = defaultConfig();
  selfHeating.tempOffsetC = -1.0f;
  Reading warm;
  warm.temperatureC = 27.0f;
  warm.humidityPct = 59.0f;
  warm.valid = VALID_TEMPERATURE | VALID_HUMIDITY;
  applyCalibration(warm, selfHeating);
  CHECK(warm.temperatureC == 26.0f);
  CHECK((warm.humidityPct > 62.4f && warm.humidityPct < 62.8f));

  // Without a valid temperature the humidity is left alone.
  Reading humidOnly;
  humidOnly.humidityPct = 59.0f;
  humidOnly.valid = VALID_HUMIDITY;
  applyCalibration(humidOnly, selfHeating);
  CHECK(humidOnly.humidityPct == 59.0f);

  Reading invalid;
  invalid.noiseDb = 40.0f;
  invalid.valid = 0;
  applyCalibration(invalid, config);
  CHECK(invalid.noiseDb == 40.0f);  // invalid metrics stay untouched
}

Config sampleConfig() {
  Config config = defaultConfig();
  config.measurementIntervalSeconds = 600;
  config.displayScreen = DISPLAY_SCREEN_BENTO;
  config.temperatureUnit = TEMPERATURE_UNIT_FAHRENHEIT;
  strncpy(config.deviceName, "Bedroom-2", sizeof config.deviceName - 1);
  config.noiseOffsetDb = -2.5f;
  config.tempOffsetC = 1.25f;
  config.humidityOffsetRh = -4.0f;
  config.frcEpochSeconds = 1767225600;
  config.frcTargetPpm = 420;
  config.frcCorrectionPpm = -37;
  return config;
}

TEST_CASE("config record codec") {
  const Config config = sampleConfig();
  uint8_t record[ConfigRecordLayout::kRecordBytes];
  encodeConfigRecord(config, 7, record);

  Config decoded;
  uint32_t sequence = 0;
  CHECK(decodeConfigRecord(record, decoded, sequence));
  CHECK_EQ(uint32_t{7}, sequence);
  CHECK_EQ(config.measurementIntervalSeconds,
           decoded.measurementIntervalSeconds);
  CHECK(strcmp(config.deviceName, decoded.deviceName) == 0);
  CHECK(decoded.noiseOffsetDb == config.noiseOffsetDb);
  CHECK(decoded.humidityOffsetRh == config.humidityOffsetRh);
  CHECK_EQ(config.frcEpochSeconds, decoded.frcEpochSeconds);
  CHECK_EQ(config.frcTargetPpm, decoded.frcTargetPpm);
  CHECK_EQ(config.frcCorrectionPpm, decoded.frcCorrectionPpm);
  CHECK_EQ(config.temperatureUnit, decoded.temperatureUnit);

  // A record written before the FRC fields or the temperature unit existed
  // has zeros there.
  uint8_t older[ConfigRecordLayout::kRecordBytes];
  encodeConfigRecord(defaultConfig(), 3, older);
  CHECK(decodeConfigRecord(older, decoded, sequence));
  CHECK_EQ(uint16_t{0}, decoded.frcTargetPpm);
  CHECK_EQ(uint8_t{TEMPERATURE_UNIT_CELSIUS}, decoded.temperatureUnit);

  uint8_t corrupted[ConfigRecordLayout::kRecordBytes];
  memcpy(corrupted, record, sizeof corrupted);
  corrupted[20] ^= 0x01;
  CHECK(!decodeConfigRecord(corrupted, decoded, sequence));
  memcpy(corrupted, record, sizeof corrupted);
  corrupted[0] ^= 0xFF;  // magic
  CHECK(!decodeConfigRecord(corrupted, decoded, sequence));
}

TEST_CASE("config store round trip and recovery") {
  FlashSim::reset();
  W25Q64Flash flash;
  flash.begin();

  ConfigStore store;
  Config loaded = defaultConfig();
  CHECK(!store.load(flash, loaded));  // blank store

  const Config saved = sampleConfig();
  CHECK(store.save(flash, saved));
  ConfigStore rebooted;
  CHECK(rebooted.load(flash, loaded));
  CHECK(strcmp(saved.deviceName, loaded.deviceName) == 0);
  CHECK_EQ(saved.measurementIntervalSeconds,
           loaded.measurementIntervalSeconds);

  // FlashDB must retain the committed value across an interrupted update.
  Config second = saved;
  second.measurementIntervalSeconds = 1800;
  FlashSim::failNextPrograms(1);
  CHECK(!rebooted.save(flash, second));
  ConfigStore recovered;
  CHECK(recovered.load(flash, loaded));
  CHECK_EQ(saved.measurementIntervalSeconds,
           loaded.measurementIntervalSeconds);

  CHECK(recovered.save(flash, second));
  ConfigStore verify;
  CHECK(verify.load(flash, loaded));
  CHECK_EQ(uint32_t{1800}, loaded.measurementIntervalSeconds);

  CHECK(!recovered.save(flash, Config{}));  // invalid config rejected
}

Reading readingForSample(uint32_t index) {
  Reading reading;
  reading.monotonicMs = 1000ULL * index;
  reading.epochMs = index == 0 ? 0 : 1752000000000ULL + 1000ULL * index;
  reading.temperatureC = 20.0f + index;
  reading.co2Ppm = 600.0f + index;
  reading.valid = VALID_TEMPERATURE | VALID_CO2;
  return reading;
}

TEST_CASE("config read failures do not erase") {
  FlashSim::reset();
  W25Q64Flash flash;
  flash.begin();
  ConfigStore store;
  const Config config = sampleConfig();
  BondTable bonds;
  uint8_t address[6] = {1}, key[16] = {2};
  bonds.storeLtk(address, key);
  CHECK(store.save(flash, config));
  CHECK(store.saveBonds(flash, bonds));
  CHECK(store.saveLogErase(flash, 201));
  Config out;
  FlashSim::resetReadCounts();
  ConfigStore baseline;
  CHECK(baseline.load(flash, out));
  const uint64_t calls = FlashSim::readCalls();
  uint8_t before[FlashStorageLayout::kConfigDbBytes];
  memcpy(before, FlashSim::data() + FlashStorageLayout::kConfigDbOffset, sizeof before);
  for (uint64_t cut = 1; cut <= calls; ++cut) {
    ConfigStore failed;
    FlashSim::failReadAt(static_cast<int>(cut));
    CHECK(!failed.load(flash, out));
    CHECK(!failed.healthy());
    CHECK(memcmp(before, FlashSim::data() + FlashStorageLayout::kConfigDbOffset,
                 sizeof before) == 0);
    FlashSim::failReadAt(0);
    CHECK(failed.load(flash, out));
    CHECK(strcmp(out.deviceName, config.deviceName) == 0);
    BondTable restored;
    CHECK(failed.loadBonds(flash, restored));
    CHECK_EQ(uint8_t{1}, restored.count());
    uint32_t next = 0;
    CHECK(failed.loadLogErase(flash, next));
    CHECK_EQ(uint32_t{201}, next);
  }
  // A corrupt nonblank header is not permission to auto-format either.
  FlashSim::data()[FlashStorageLayout::kConfigDbOffset + 8] ^= 0x01;
  memcpy(before, FlashSim::data() + FlashStorageLayout::kConfigDbOffset, sizeof before);
  ConfigStore corrupt;
  CHECK(!corrupt.load(flash, out));
  CHECK(memcmp(before, FlashSim::data() + FlashStorageLayout::kConfigDbOffset,
               sizeof before) == 0);
}

TEST_CASE("sample log readout") {
  FlashSim::reset();
  W25Q64Flash flash;
  flash.begin();

  SampleLog log;
  for (uint32_t i = 0; i < 10; i++) {
    CHECK(log.append(flash, readingForSample(i)));
  }

  // A rebooted (freshly mounted) log must stream the same records.
  SampleLog rebooted;
  SampleLogCursor cursor;
  CHECK(rebooted.startRead(flash, 0, cursor));
  SampleRecord record;
  for (uint32_t expected = 1; expected <= 10; expected++) {
    CHECK(rebooted.readNext(flash, cursor, record));
    CHECK_EQ(expected, record.sequence);
  }
  CHECK(!rebooted.readNext(flash, cursor, record));  // caught up
  CHECK(cursor.active);

  // Partial sync from a cursor the app remembered.
  CHECK(rebooted.startRead(flash, 6, cursor));
  CHECK(rebooted.readNext(flash, cursor, record));
  CHECK_EQ(uint32_t{6}, record.sequence);
  CHECK_EQ(uint32_t{1752000005}, record.epochSeconds);  // sample index 5

  // A start beyond the head is immediately caught up.
  CHECK(rebooted.startRead(flash, 99, cursor));
  CHECK(!rebooted.readNext(flash, cursor, record));
  CHECK(cursor.active);
}

TEST_CASE("sample log readout skips torn slot") {
  FlashSim::reset();
  W25Q64Flash flash;
  flash.begin();

  SampleLog log;
  for (uint32_t i = 0; i < 5; i++) {
    CHECK(log.append(flash, readingForSample(i)));
  }
  FlashSim::failNextPrograms(1);
  CHECK(!log.append(flash, readingForSample(5)));  // torn slot
  CHECK(log.append(flash, readingForSample(5)));   // retries atomically

  SampleLogCursor cursor;
  SampleRecord record;
  CHECK(log.startRead(flash, 0, cursor));
  for (uint32_t expected = 1; expected <= 6; expected++) {
    CHECK(log.readNext(flash, cursor, record));
    CHECK_EQ(expected, record.sequence);
  }
  CHECK(!log.readNext(flash, cursor, record));

  // startRead must pick the later sector when it already covers the start.
  CHECK(log.startRead(flash, 6, cursor));
  CHECK(log.readNext(flash, cursor, record));
  CHECK_EQ(uint32_t{6}, record.sequence);
}

TEST_CASE("sample log recovers every commit stage") {
  for (int cut = 1; cut <= 4; ++cut) {
    FlashSim::reset();
    W25Q64Flash flash; flash.begin();
    SampleLog log;
    CHECK(log.append(flash, readingForSample(1)));
    FlashSim::failProgramAt(cut);
    CHECK(!log.append(flash, readingForSample(2)));
    CHECK(log.append(flash, readingForSample(3)));
    CHECK(log.append(flash, readingForSample(4)));
    SampleLogCursor cursor; SampleRecord out;
    CHECK(log.startRead(flash, 0, cursor));
    uint32_t previous = 0;
    bool saw3 = false, saw4 = false;
    while (log.readNext(flash, cursor, out)) {
      CHECK(out.sequence > previous);
      previous = out.sequence;
      saw3 |= out.monotonicMs == 3000;
      saw4 |= out.monotonicMs == 4000;
    }
    CHECK((cursor.active && saw3 && saw4));
  }
}

TEST_CASE("sample log linear read and errors") {
  FlashSim::reset();
  W25Q64Flash flash; flash.begin(); SampleLog log;
  // More than a whole ring: cover physical wrap as well as linear cost.
  for (uint32_t i = 1; i <= 110000; ++i)
    CHECK(log.append(flash, readingForSample(i)));
  SampleLogCursor cursor; SampleRecord out;
  CHECK(log.startRead(flash, 0, cursor));
  FlashSim::resetReadCounts();
  uint32_t count = 0, previous = 0;
  while (log.readNext(flash, cursor, out)) {
    CHECK(out.sequence > previous);
    previous = out.sequence; ++count;
  }
  CHECK(cursor.active);
  CHECK((count > 90000 && count < 110000));
  CHECK_EQ(uint32_t{110000}, previous);
  CHECK(FlashSim::readCalls() < uint64_t{count} * 3 + 5000);
  CHECK(FlashSim::readBytes() < uint64_t{count} * 100 + 300000);
  std::cout << "Full-ring download: " << count << " records, "
            << FlashSim::readBytes() << " flash bytes\n";
  for (int read = 1; read <= 3; ++read) {
    CHECK(log.startRead(flash, 0, cursor));
    FlashSim::failReadAt(read); // sector header, index, then payload
    CHECK(!log.readNext(flash, cursor, out));
    CHECK(!cursor.active);
  }
  CHECK(log.startRead(flash, 0, cursor));
  CHECK(log.append(flash, readingForSample(110001)));
  CHECK(!log.readNext(flash, cursor, out));
  CHECK(!cursor.active); // no stale physical pointer after mutation
}

void expectSealedPacket(const uint8_t* packet, uint16_t length) {
  uint8_t copy[BleCodec::kMaxLogPacketBytes];
  memcpy(copy, packet, length);
  const uint8_t stored = copy[11];
  copy[11] = 0;
  CHECK_EQ(stored, BleCodec::crc8(copy, length));
}

TEST_CASE("BLE codec decoders") {
  uint32_t seconds = 0;
  uint8_t wire[16] = {};
  LittleEndian::putU32(wire, 600);
  CHECK(BleCodec::decodeIntervalWrite(wire, 4, seconds));
  CHECK_EQ(uint32_t{600}, seconds);
  LittleEndian::putU32(wire, 120);
  CHECK(!BleCodec::decodeIntervalWrite(wire, 4, seconds));
  CHECK(!BleCodec::decodeIntervalWrite(wire, 3, seconds));

  uint64_t epochMs = 0;
  LittleEndian::putU64(wire, 1752000000000ULL);
  CHECK(BleCodec::decodeEpochWrite(wire, 8, epochMs));
  LittleEndian::putU64(wire, 0);
  CHECK(!BleCodec::decodeEpochWrite(wire, 8, epochMs));
  LittleEndian::putU64(wire, 0xFFFFFFFFFFFFFFFFULL);  // epochSeconds overflow
  CHECK(!BleCodec::decodeEpochWrite(wire, 8, epochMs));

  uint8_t screen = 0;
  wire[0] = DISPLAY_SCREEN_BENTO;
  CHECK(BleCodec::decodeScreenWrite(wire, 1, screen));
  wire[0] = 3;
  CHECK(!BleCodec::decodeScreenWrite(wire, 1, screen));

  float noise = 0, temp = 0, rh = 0;
  LittleEndian::putF32(wire + 0, -3.0f);
  LittleEndian::putF32(wire + 4, 2.0f);
  LittleEndian::putF32(wire + 8, -5.0f);
  CHECK(BleCodec::decodeCalibrationOffsets(wire, 12, noise, temp, rh));
  CHECK((noise == -3.0f && temp == 2.0f && rh == -5.0f));
  LittleEndian::putF32(wire + 4, 100.0f);  // out of bounds
  CHECK(!BleCodec::decodeCalibrationOffsets(wire, 12, noise, temp, rh));

  uint16_t target = 0;
  wire[0] = BleCodec::kCalOpForcedRecalibration;
  wire[1] = 0;
  LittleEndian::putU16(wire + 2, 420);
  CHECK(BleCodec::decodeCalibrationControl(wire, 4, target));
  CHECK_EQ(uint16_t{420}, target);
  LittleEndian::putU16(wire + 2, 5000);
  CHECK(!BleCodec::decodeCalibrationControl(wire, 4, target));
  wire[0] = 9;
  LittleEndian::putU16(wire + 2, 420);
  CHECK(!BleCodec::decodeCalibrationControl(wire, 4, target));

  char name[Config::kDeviceNameMaxLength + 1];
  const uint8_t good[] = {'S', 'a', 'l', 'o', 'n'};
  CHECK(BleCodec::decodeDeviceName(good, sizeof good, name));
  CHECK(strcmp(name, "Salon") == 0);
  const uint8_t control[] = {'a', 0x07, 'b'};
  CHECK(!BleCodec::decodeDeviceName(control, sizeof control, name));
  const uint8_t embeddedNul[] = {'a', 0x00, 'b'};
  CHECK(!BleCodec::decodeDeviceName(embeddedNul, sizeof embeddedNul,
                                    name));
  uint8_t tooLong[17];
  memset(tooLong, 'x', sizeof tooLong);
  CHECK(!BleCodec::decodeDeviceName(tooLong, sizeof tooLong, name));
  CHECK(!BleCodec::decodeDeviceName(good, 0, name));

  BleCodec::LogSyncRequest request;
  wire[0] = BleCodec::kSyncOpStart;
  wire[1] = 0;
  LittleEndian::putU32(wire + 2, 1234);
  LittleEndian::putU16(wire + 6, 500);
  LittleEndian::putU16(wire + 8, 180);
  CHECK(BleCodec::decodeLogSyncControl(wire, 10, request));
  CHECK_EQ(uint32_t{1234}, request.startSequence);
  CHECK_EQ(uint16_t{500}, request.maxRecords);
  CHECK_EQ(uint16_t{180}, request.attPayload);
  wire[0] = 7;
  CHECK(!BleCodec::decodeLogSyncControl(wire, 10, request));
}

TEST_CASE("BLE codec encoders") {
  Reading reading;
  reading.temperatureC = 23.46f;
  reading.humidityPct = 45.5f;
  reading.pressurePa = 101325.0f;
  reading.co2Ppm = 70000.0f;  // saturates
  reading.lux = 210.0f;
  reading.noiseDb = -12.34f;
  reading.batteryV = 3.987f;
  reading.valid = VALID_TEMPERATURE | VALID_CO2 | VALID_BATTERY;
  uint8_t out[BleCodec::kReadingBytes];
  BleCodec::encodeReadingCompact(reading, out);
  CHECK_EQ(int16_t{2346}, LittleEndian::getI16(out + 0));
  CHECK_EQ(uint16_t{4550}, LittleEndian::getU16(out + 2));
  CHECK_EQ(uint32_t{101325}, LittleEndian::getU32(out + 4));
  CHECK_EQ(uint16_t{65535}, LittleEndian::getU16(out + 8));
  CHECK_EQ(uint32_t{2100}, LittleEndian::getU32(out + 10));
  CHECK_EQ(int16_t{-123}, LittleEndian::getI16(out + 14));
  CHECK_EQ(uint16_t{3987}, LittleEndian::getU16(out + 16));
  CHECK_EQ(uint8_t(VALID_TEMPERATURE | VALID_CO2 | VALID_BATTERY), out[18]);

  FaultStatus faults;
  faults.scd41.present = true;
  faults.scd41.timedOut = true;
  faults.scd41.consecutiveFailures = 300;  // saturates to 255
  faults.flash.present = true;
  BleCodec::StatusInfo info;
  info.faults = &faults;
  info.validFlags = VALID_CO2;
  info.lastSequence = 4242;
  info.timeSynced = true;
  info.syncActive = false;
  info.logErasing = false;
  info.charging = true;
  info.frcState = BleCodec::kFrcDone;
  info.frcCorrectionPpm = -17;
  uint8_t status[BleCodec::kStatusBytes];
  BleCodec::encodeStatus(info, status);
  CHECK_EQ(uint16_t{VALID_CO2}, LittleEndian::getU16(status + 0));
  CHECK_EQ(uint8_t{(1u << 2) | (1u << 6)}, status[2]);  // scd41 + flash
  CHECK_EQ(uint8_t{1u << 2}, status[3]);
  CHECK_EQ(uint8_t{255}, status[4 + 2]);
  CHECK_EQ(uint32_t{4242}, LittleEndian::getU32(status + 12));
  CHECK_EQ(uint8_t{1 | 8}, status[16]);  // clock synced + charging
  CHECK_EQ(uint8_t{BleCodec::kFrcDone}, status[17]);
  CHECK_EQ(int16_t{-17}, LittleEndian::getI16(status + 18));
}

SampleRecord wireTestRecord(uint32_t sequence) {
  Reading reading = readingForSample(sequence);
  return makeSampleRecord(reading, sequence, 0);
}

TEST_CASE("BLE codec packets") {
  // CRC-8 poly 0x07 reference vector.
  CHECK_EQ(uint8_t{0xF4},
           BleCodec::crc8(reinterpret_cast<const uint8_t*>("123456789"), 9));

  CHECK_EQ(uint16_t{20}, BleCodec::clampAttPayload(0));
  CHECK_EQ(uint16_t{20}, BleCodec::clampAttPayload(5));
  CHECK_EQ(uint16_t{180}, BleCodec::clampAttPayload(512));
  CHECK_EQ(uint8_t{0}, BleCodec::recordsPerPacket(20));
  CHECK_EQ(uint8_t{0}, BleCodec::recordsPerPacket(63));
  CHECK_EQ(uint8_t{1}, BleCodec::recordsPerPacket(64));
  CHECK_EQ(uint8_t{3}, BleCodec::recordsPerPacket(180));
  CHECK_EQ(uint8_t{7}, BleCodec::fragmentsPerRecord(20));

  SampleRecord records[3] = {wireTestRecord(10), wireTestRecord(11),
                             wireTestRecord(12)};
  uint8_t packet[BleCodec::kMaxLogPacketBytes];
  uint16_t length = BleCodec::buildLogDataPacket(3, 40, records, 3, packet);
  CHECK_EQ(uint16_t{12 + 3 * 52}, length);
  CHECK_EQ(uint8_t{BleCodec::kLogPacketData}, packet[0]);
  CHECK_EQ(uint8_t{3}, packet[1]);
  CHECK_EQ(uint32_t{10}, LittleEndian::getU32(packet + 2));
  CHECK_EQ(uint8_t{3}, packet[6]);
  CHECK_EQ(uint16_t{40}, LittleEndian::getU16(packet + 8));
  expectSealedPacket(packet, length);
  CHECK_EQ(uint32_t{11}, LittleEndian::getU32(packet + 12 + 52));

  // Fragment path at the MTU floor: 6 chunks of 8 bytes and one of 4
  // reassemble the wire record exactly.
  uint8_t wire[BleCodec::kWireRecordBytes];
  BleCodec::sampleRecordToWire(records[0], wire);
  CHECK_EQ(uint32_t{10}, LittleEndian::getU32(wire + 0));
  CHECK_EQ(uint16_t{1}, LittleEndian::getU16(wire + 6));  // timeSynced flag
  uint8_t reassembled[BleCodec::kWireRecordBytes];
  for (uint8_t frag = 0; frag < 7; frag++) {
    length = BleCodec::buildLogFragmentPacket(frag, 40, wire, 10, frag, 20,
                                              packet);
    CHECK_EQ(static_cast<uint16_t>(frag < 6 ? 20 : 16), length);
    CHECK_EQ(uint8_t{BleCodec::kLogPacketFragment}, packet[0]);
    CHECK_EQ(uint8_t{frag}, packet[7]);
    expectSealedPacket(packet, length);
    memcpy(reassembled + frag * 8, packet + 12, length - 12);
  }
  CHECK(memcmp(wire, reassembled, sizeof wire) == 0);
  CHECK_EQ(uint16_t{0},
           BleCodec::buildLogFragmentPacket(7, 40, wire, 10, 7, 20, packet));

  length = BleCodec::buildLogEndPacket(9, 43, packet);
  CHECK_EQ(uint16_t{12}, length);
  CHECK_EQ(uint8_t{BleCodec::kLogPacketEnd}, packet[0]);
  CHECK_EQ(uint32_t{43}, LittleEndian::getU32(packet + 2));
  CHECK_EQ(uint8_t{0}, packet[6]);
  expectSealedPacket(packet, length);
}


TEST_CASE("unit identity") {
  char serial[UnitIdentity::kSerialChars + 1];
  UnitIdentity::formatSerial(0x0123456789ABCDEFULL, serial);
  CHECK(strcmp(serial, "0123456789ABCDEF") == 0);
  UnitIdentity::formatSerial(0, serial);
  CHECK(strcmp(serial, "0000000000000000") == 0);

  const Config config = UnitIdentity::defaultConfigForUnit(0x0123456789ABCDEFULL);
  CHECK(strcmp(config.deviceName, "Quiesco CDEF") == 0);
  CHECK(isValidConfig(config));
  CHECK_EQ(uint32_t{Config::kDefaultIntervalSeconds},
           config.measurementIntervalSeconds);
}

void fillBytes(uint8_t* out, uint8_t count, uint8_t seed) {
  for (uint8_t i = 0; i < count; i++) {
    out[i] = static_cast<uint8_t>(seed + i);
  }
}

TEST_CASE("bond table") {
  BondTable table;
  uint8_t address[6];
  uint8_t key[16];
  uint8_t found[16];
  CHECK_EQ(uint8_t{0}, table.count());

  // One phone: IRK then LTK for the same identity address share a slot.
  fillBytes(address, 6, 0x10);
  fillBytes(key, 16, 0xA0);
  table.storeIrk(address, key);
  fillBytes(key, 16, 0xB0);
  table.storeLtk(address, key);
  CHECK_EQ(uint8_t{1}, table.count());
  CHECK_EQ(uint8_t{1}, table.irkCount());
  CHECK(table.findLtk(address, found));
  CHECK(memcmp(found, key, 16) == 0);
  CHECK_EQ(uint8_t{0xA0}, table.irkBond(0)->irk[0]);

  // Fill the table, then one more evicts the oldest (the first phone).
  for (uint8_t phone = 1; phone <= BondTable::kMaxBonds; phone++) {
    fillBytes(address, 6, static_cast<uint8_t>(0x20 * phone));
    fillBytes(key, 16, phone);
    table.storeLtk(address, key);
  }
  CHECK_EQ(uint8_t{BondTable::kMaxBonds}, table.count());
  fillBytes(address, 6, 0x10);
  CHECK(!table.findLtk(address, found));
  fillBytes(address, 6, 0x20);  // phone 1 survives, now the oldest
  CHECK(table.findLtk(address, found));
  CHECK_EQ(uint8_t{1}, found[0]);

  // Encoding keeps the bonds and their eviction order.
  uint8_t encoded[BondTable::kEncodedBytes];
  table.encode(encoded);
  BondTable restored;
  CHECK(restored.decode(encoded));
  CHECK_EQ(uint8_t{BondTable::kMaxBonds}, restored.count());
  CHECK(restored.findLtk(address, found));
  fillBytes(address, 6, 0x30);
  fillBytes(key, 16, 9);
  restored.storeLtk(address, key);  // evicts phone 1 again
  fillBytes(address, 6, 0x20);
  CHECK(!restored.findLtk(address, found));

  encoded[5] ^= 0x01;
  CHECK(!restored.decode(encoded));  // CRC
  CHECK_EQ(uint8_t{BondTable::kMaxBonds}, restored.count());  // untouched

  table.clear();
  CHECK_EQ(uint8_t{0}, table.count());
}

TEST_CASE("bond persistence") {
  FlashSim::reset();
  W25Q64Flash flash;
  flash.begin();

  ConfigStore store;
  BondTable bonds;
  CHECK(!store.loadBonds(flash, bonds));  // blank
  uint8_t address[6];
  uint8_t key[16];
  fillBytes(address, 6, 0x42);
  fillBytes(key, 16, 0x77);
  bonds.storeLtk(address, key);
  CHECK(store.saveBonds(flash, bonds));
  CHECK(store.save(flash, sampleConfig()));

  ConfigStore rebooted;
  BondTable loaded;
  CHECK(rebooted.loadBonds(flash, loaded));
  uint8_t found[16];
  CHECK(loaded.findLtk(address, found));
  CHECK_EQ(uint8_t{0x77}, found[0]);
  Config config = defaultConfig();
  CHECK(rebooted.load(flash, config));  // config unaffected
}

TEST_CASE("sample record SCD41 temperature and humidity") {
  Reading reading = readingForSample(1);
  SampleRecord plain = makeSampleRecord(reading, 1, 1);
  CHECK_EQ(uint8_t{0}, plain.flags);  // not recorded

  reading.scdTemperatureC = 28.37f;
  reading.scdHumidityPct = 55.5f;
  reading.scdRhtValid = true;
  const SampleRecord record = makeSampleRecord(reading, 2, 1);
  CHECK(isValidSampleRecord(record));
  CHECK_EQ(uint8_t{kSampleScdRht}, record.flags);
  CHECK_EQ(int16_t{2837}, record.scdTemperatureCenti);
  CHECK_EQ(uint16_t{5550}, record.scdHumidityCenti);

  // The wire record (BLE) is unchanged: the comparison is a log-dump detail.
  uint8_t withRht[BleCodec::kWireRecordBytes];
  uint8_t without[BleCodec::kWireRecordBytes];
  BleCodec::sampleRecordToWire(record, withRht);
  SampleRecord stripped = record;
  stripped.flags = 0;
  stripped.scdTemperatureCenti = 0;
  stripped.scdHumidityCenti = 0;
  BleCodec::sampleRecordToWire(stripped, without);
  CHECK(memcmp(withRht, without, sizeof withRht) == 0);
}

TEST_CASE("boot counter") {
  FlashSim::reset();
  W25Q64Flash flash;
  flash.begin();

  uint32_t boot = 0;
  ConfigStore store;
  CHECK(store.advanceBootCount(flash, boot));
  CHECK_EQ(uint32_t{1}, boot);  // blank store
  CHECK(store.save(flash, sampleConfig()));  // shares the KV store

  ConfigStore rebooted;
  CHECK(rebooted.advanceBootCount(flash, boot));
  CHECK_EQ(uint32_t{2}, boot);
  Config loaded = defaultConfig();
  CHECK(rebooted.load(flash, loaded));
  CHECK_EQ(uint32_t{600}, loaded.measurementIntervalSeconds);

  // Records carry the boot they were taken in.
  SampleLog log;
  log.setBootCount(boot);
  CHECK(log.append(flash, readingForSample(1)));
  SampleLogCursor cursor;
  SampleRecord record;
  CHECK(log.startRead(flash, 0, cursor));
  CHECK(log.readNext(flash, cursor, record));
  CHECK_EQ(uint32_t{2}, record.bootCount);
  CHECK(isValidSampleRecord(record));
}

TEST_CASE("sample log erase") {
  FlashSim::reset();
  W25Q64Flash flash;
  flash.begin();

  SampleLog log;
  for (uint32_t i = 0; i < 10; i++) {
    CHECK(log.append(flash, readingForSample(i)));
  }
  log.beginErase();
  CHECK(log.erasing());
  CHECK(!log.append(flash, readingForSample(10)));  // unmounted meanwhile
  uint32_t steps = 0;
  SampleLog::EraseProgress progress;
  do {
    progress = log.eraseStep(flash);
    steps++;
  } while (progress == SampleLog::EraseProgress::kRunning);
  CHECK(progress == SampleLog::EraseProgress::kDone);
  CHECK_EQ(FlashStorageLayout::kSampleDbBytes / FlashStorageLayout::kSectorBytes,
           steps);
  CHECK(!log.erasing());
  for (uint32_t address = FlashStorageLayout::kSampleDbOffset;
       address < FlashStorageLayout::kSampleDbEnd; address += 997) {
    if (FlashSim::data()[address] != 0xFF) {
      FAIL_CHECK("sample partition not erased at " << address);
      break;
    }
  }

  // Old records are gone, and the next sequence carries on from 10.
  SampleLogCursor cursor;
  SampleRecord record;
  CHECK(log.startRead(flash, 0, cursor));
  CHECK(!log.readNext(flash, cursor, record));
  CHECK(log.append(flash, readingForSample(10)));
  CHECK_EQ(uint32_t{11}, log.lastSequence());

  SampleLog rebooted;
  CHECK(rebooted.startRead(flash, 0, cursor));
  CHECK(rebooted.readNext(flash, cursor, record));
  CHECK_EQ(uint32_t{11}, record.sequence);
  CHECK(!rebooted.readNext(flash, cursor, record));
  CHECK_EQ(uint32_t{11}, rebooted.lastSequence());
}

TEST_CASE("BLE codec control") {
  uint8_t wire[BleCodec::kCoreConfigBytes];
  Config config = defaultConfig();
  config.measurementIntervalSeconds = 60;
  BleCodec::encodeCoreConfig(config, wire);
  Config decoded = defaultConfig();
  strcpy(decoded.deviceName, "Kept");
  CHECK(BleCodec::decodeCoreConfig(wire, sizeof wire, decoded));
  CHECK_EQ(uint32_t{60}, decoded.measurementIntervalSeconds);
  CHECK(strcmp(decoded.deviceName, "Kept") == 0);  // not a core field
  CHECK(!BleCodec::decodeCoreConfig(wire, 15, decoded));
  LittleEndian::putU32(wire, Config::kCurrentVersion + 1);  // stale version
  CHECK(!BleCodec::decodeCoreConfig(wire, sizeof wire, decoded));
  LittleEndian::putU32(wire, Config::kCurrentVersion);
  wire[15] = 1;  // reserved byte
  CHECK(!BleCodec::decodeCoreConfig(wire, sizeof wire, decoded));
  wire[15] = 0;
  wire[14] = TEMPERATURE_UNIT_FAHRENHEIT;
  CHECK(BleCodec::decodeCoreConfig(wire, sizeof wire, decoded));
  CHECK_EQ(uint8_t{TEMPERATURE_UNIT_FAHRENHEIT}, decoded.temperatureUnit);
  wire[14] = 2;  // no such unit
  CHECK(!BleCodec::decodeCoreConfig(wire, sizeof wire, decoded));
  CHECK_EQ(uint8_t{TEMPERATURE_UNIT_FAHRENHEIT}, decoded.temperatureUnit);
  wire[14] = 0;
  LittleEndian::putU32(wire + 4, 61);
  CHECK(!BleCodec::decodeCoreConfig(wire, sizeof wire, decoded));
  CHECK_EQ(uint32_t{60}, decoded.measurementIntervalSeconds);  // untouched

  BleCodec::DeviceControlRequest request = {};
  uint8_t control[9] = {BleCodec::kControlFactoryReset, 0, 0, 0};
  LittleEndian::putU16(control + 2, BleCodec::kFactoryResetConfirm);
  CHECK(BleCodec::decodeDeviceControl(control, 4, request));
  CHECK_EQ(uint8_t{BleCodec::kControlFactoryReset}, request.opcode);
  CHECK(!request.eraseLog);
  control[1] = BleCodec::kResetEraseLog;
  CHECK(BleCodec::decodeDeviceControl(control, 4, request));
  CHECK(request.eraseLog);
  control[1] = 0x02;  // unknown flag
  CHECK(!BleCodec::decodeDeviceControl(control, 4, request));
  control[1] = 0;
  LittleEndian::putU16(control + 2, 0xFAC6);  // wrong confirmation
  CHECK(!BleCodec::decodeDeviceControl(control, 4, request));
  LittleEndian::putU16(control + 2, BleCodec::kFactoryResetConfirm);
  control[0] = 3;  // no such opcode
  CHECK(!BleCodec::decodeDeviceControl(control, 4, request));
  CHECK(!BleCodec::decodeDeviceControl(control, 8, request));
  control[0] = BleCodec::kControlFactoryReset;
  CHECK(!BleCodec::decodeDeviceControl(control, 3, request));
  CHECK(!BleCodec::decodeDeviceControl(control, 8, request));  // reset is 4

  // Log erase: 8 bytes, reserved flags, the same confirmation.
  control[0] = BleCodec::kControlEraseLog;
  LittleEndian::putU32(control + 4, 1443);
  CHECK(BleCodec::decodeDeviceControl(control, 8, request));
  CHECK_EQ(uint8_t{BleCodec::kControlEraseLog}, request.opcode);
  CHECK_EQ(uint32_t{1443}, request.upToSequence);
  CHECK(!BleCodec::decodeDeviceControl(control, 4, request));  // too short
  CHECK(!BleCodec::decodeDeviceControl(control, 7, request));
  CHECK(!BleCodec::decodeDeviceControl(control, 9, request));  // ArduinoBLE spare
  control[1] = BleCodec::kResetEraseLog;  // flags are reserved here
  CHECK(!BleCodec::decodeDeviceControl(control, 8, request));
  control[1] = 0;
  LittleEndian::putU16(control + 2, 0xFAC6);
  CHECK(!BleCodec::decodeDeviceControl(control, 8, request));
  LittleEndian::putU16(control + 2, BleCodec::kFactoryResetConfirm);
  LittleEndian::putU32(control + 4, 0);  // up to 0: only an empty log
  CHECK(BleCodec::decodeDeviceControl(control, 8, request));
  CHECK_EQ(uint32_t{0}, request.upToSequence);
}

// Frozen wire layouts. Each vector is also a worked example in PROTOCOL.md,
// and the test fails unless the document contains it verbatim, so the codec,
// the reference and kProtocolVersion can only change together.
std::string toHex(const uint8_t* data, uint16_t length) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  for (uint16_t i = 0; i < length; i++) {
    if (i != 0) {
      out += ' ';
    }
    out += kHex[data[i] >> 4];
    out += kHex[data[i] & 0xF];
  }
  return out;
}

// The document with every whitespace run collapsed to one space, so its
// examples may wrap anywhere.
std::string normalizedProtocolDoc() {
  std::ifstream file(QUIESCO_PROTOCOL_DOC);
  std::stringstream raw;
  raw << file.rdbuf();
  std::string out;
  bool space = false;
  for (const char c : raw.str()) {
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
      space = true;
      continue;
    }
    if (space && !out.empty()) {
      out += ' ';
    }
    space = false;
    out += c;
  }
  return out;
}

void expectVector(const std::string& doc, const char* name,
                  const uint8_t* data, uint16_t length, const char* expected) {
  CAPTURE(name);
  CHECK_EQ(std::string(expected), toHex(data, length));
  CHECK_MESSAGE(doc.find(expected) != std::string::npos,
                "golden vector missing from PROTOCOL.md");
}

TEST_CASE("protocol golden vectors") {
  const std::string doc = normalizedProtocolDoc();
  CHECK(!doc.empty());
  CHECK_EQ(uint16_t{6}, BleCodec::kProtocolVersion);
  CHECK(doc.find("Protocol version: 6") != std::string::npos);

  BleCodec::DeviceInfo deviceInfo;
  deviceInfo.firmwareMajor = 1;
  deviceInfo.firmwareMinor = 2;
  deviceInfo.firmwarePatch = 3;
  deviceInfo.debugBuild = false;
  deviceInfo.pairingOpen = true;
  deviceInfo.bonded = false;
  deviceInfo.noBattery = false;
  deviceInfo.scd41Serial = 0xA1B2C3D4E5F6ULL;
  deviceInfo.bootCount = 7;
  uint8_t info[BleCodec::kDeviceInfoBytes];
  BleCodec::encodeDeviceInfo(deviceInfo, info);
  expectVector(doc, "device info", info, sizeof info, GOLDEN_DEVICE_INFO);
  deviceInfo.noBattery = true;
  BleCodec::encodeDeviceInfo(deviceInfo, info);
  CHECK_EQ(uint8_t{2 | 8}, info[9]);  // enrolment open + no battery

  uint8_t interval[4];
  LittleEndian::putU32(interval, 300);
  expectVector(doc, "interval", interval, sizeof interval, GOLDEN_INTERVAL);

  uint8_t config[BleCodec::kCoreConfigBytes];
  BleCodec::encodeCoreConfig(defaultConfig(), config);
  expectVector(doc, "core config", config, sizeof config, GOLDEN_CORE_CONFIG);

  Reading reading;
  reading.temperatureC = 21.5f;
  reading.humidityPct = 45.25f;
  reading.pressurePa = 101325.0f;
  reading.co2Ppm = 612.0f;
  reading.lux = 180.4f;
  reading.noiseDb = 38.6f;
  reading.batteryV = 3.912f;
  reading.valid = 0x7F;
  uint8_t compact[BleCodec::kReadingBytes];
  BleCodec::encodeReadingCompact(reading, compact);
  expectVector(doc, "reading", compact, sizeof compact, GOLDEN_READING);

  FaultStatus faults;
  DeviceFault* devices[] = {&faults.bme280,     &faults.veml7700,
                            &faults.scd41,      &faults.battery,
                            &faults.microphone, &faults.display,
                            &faults.flash,      &faults.ble};
  for (DeviceFault* device : devices) {
    device->present = true;
  }
  faults.veml7700.present = false;
  faults.veml7700.consecutiveFailures = 3;
  BleCodec::StatusInfo statusInfo;
  statusInfo.faults = &faults;
  statusInfo.validFlags = 0x6F;  // everything but light
  statusInfo.lastSequence = 1234;
  statusInfo.timeSynced = true;
  statusInfo.syncActive = false;
  statusInfo.logErasing = false;
  statusInfo.charging = false;
  statusInfo.frcState = BleCodec::kFrcDone;
  statusInfo.frcCorrectionPpm = -23;
  uint8_t status[BleCodec::kStatusBytes];
  BleCodec::encodeStatus(statusInfo, status);
  expectVector(doc, "status", status, sizeof status, GOLDEN_STATUS);

  uint8_t epoch[8];
  LittleEndian::putU64(epoch, 1767225600000ULL);  // 2026-01-01T00:00:00Z
  expectVector(doc, "epoch", epoch, sizeof epoch, GOLDEN_EPOCH);

  Config offsetsConfig = defaultConfig();
  offsetsConfig.noiseOffsetDb = -1.5f;
  offsetsConfig.tempOffsetC = -2.0f;
  offsetsConfig.humidityOffsetRh = 3.0f;
  uint8_t offsets[BleCodec::kCalibrationOffsetsBytes];
  BleCodec::encodeCalibrationOffsets(offsetsConfig, offsets);
  expectVector(doc, "offsets", offsets, sizeof offsets, GOLDEN_OFFSETS);

  uint8_t calibrationState[BleCodec::kCalibrationStateBytes];
  BleCodec::encodeCalibrationState(sampleConfig(), true, calibrationState);
  expectVector(doc, "calibration state", calibrationState,
               sizeof calibrationState, GOLDEN_CALIBRATION_STATE);

  BleCodec::Diagnostics diagnostics;
  diagnostics.resetReason = 1u << 1;  // watchdog
  diagnostics.uptimeSeconds = 3600;
  diagnostics.i2cBusStuck = false;
  uint8_t diagnosticsWire[BleCodec::kDiagnosticsBytes];
  BleCodec::encodeDiagnostics(diagnostics, diagnosticsWire);
  expectVector(doc, "diagnostics", diagnosticsWire, sizeof diagnosticsWire,
               GOLDEN_DIAGNOSTICS);

  const uint8_t sync[BleCodec::kLogSyncControlBytes] = {
      BleCodec::kSyncOpStart, 0, 0xB1, 0x04, 0, 0, 0, 0, 0xF4, 0x00};
  BleCodec::LogSyncRequest request;
  CHECK(BleCodec::decodeLogSyncControl(sync, sizeof sync, request));
  CHECK_EQ(uint32_t{1201}, request.startSequence);
  CHECK_EQ(uint16_t{0}, request.maxRecords);
  CHECK_EQ(uint16_t{244}, request.attPayload);
  expectVector(doc, "sync start", sync, sizeof sync, GOLDEN_SYNC_START);

  const uint8_t frc[BleCodec::kCalibrationControlBytes] = {
      BleCodec::kCalOpForcedRecalibration, 0, 0xA4, 0x01};
  uint16_t targetPpm = 0;
  CHECK(BleCodec::decodeCalibrationControl(frc, sizeof frc, targetPpm));
  CHECK_EQ(uint16_t{420}, targetPpm);
  expectVector(doc, "frc", frc, sizeof frc, GOLDEN_FRC);

  const uint8_t reset[BleCodec::kDeviceControlBytes] = {
      BleCodec::kControlFactoryReset, BleCodec::kResetEraseLog, 0xC7, 0xFA};
  BleCodec::DeviceControlRequest control = {};
  CHECK(BleCodec::decodeDeviceControl(reset, sizeof reset, control));
  CHECK_EQ(uint8_t{BleCodec::kControlFactoryReset}, control.opcode);
  CHECK(control.eraseLog);
  expectVector(doc, "factory reset", reset, sizeof reset, GOLDEN_RESET);

  uint8_t erase[BleCodec::kEraseLogControlBytes] = {BleCodec::kControlEraseLog,
                                                    0};
  LittleEndian::putU16(erase + 2, BleCodec::kFactoryResetConfirm);
  LittleEndian::putU32(erase + 4, 1443);
  CHECK(BleCodec::decodeDeviceControl(erase, sizeof erase, control));
  CHECK_EQ(uint8_t{BleCodec::kControlEraseLog}, control.opcode);
  CHECK_EQ(uint32_t{1443}, control.upToSequence);
  expectVector(doc, "log erase", erase, sizeof erase, GOLDEN_ERASE_LOG);

  SleepWindow window = defaultSleepWindow();
  uint8_t sleep[BleCodec::kSleepWindowBytes];
  BleCodec::encodeSleepWindow(window, sleep);
  expectVector(doc, "sleep window default", sleep, sizeof sleep,
               GOLDEN_SLEEP_WINDOW_DEFAULT);
  window.flags = SleepWindow::kFlagFollowApp;
  window.utcOffsetMinutes = 120;
  BleCodec::encodeSleepWindow(window, sleep);
  expectVector(doc, "sleep window", sleep, sizeof sleep, GOLDEN_SLEEP_WINDOW);
  SleepWindow decoded = defaultSleepWindow();
  CHECK(BleCodec::decodeSleepWindow(sleep, sizeof sleep, decoded));
  CHECK(decoded.followsApp());
  CHECK_EQ(int16_t{120}, decoded.utcOffsetMinutes);
  CHECK_EQ(uint16_t{1410}, decoded.weekdayBedMin);
  CHECK_EQ(uint16_t{420}, decoded.weekdayWakeMin);
  CHECK(!decoded.hasWeekend());

  Reading logged = reading;
  logged.monotonicMs = 3600000;
  logged.epochMs = 1767229200000ULL;  // 2026-01-01T01:00:00Z
  const SampleRecord record = makeSampleRecord(logged, 1201, 7);
  uint8_t wire[BleCodec::kWireRecordBytes];
  BleCodec::sampleRecordToWire(record, wire);
  expectVector(doc, "wire record", wire, sizeof wire, GOLDEN_WIRE_RECORD);

  uint8_t packet[BleCodec::kMaxLogPacketBytes];
  uint16_t length =
      BleCodec::buildLogDataPacket(0, 1, &record, 1, packet);
  CHECK_EQ(uint16_t{64}, length);
  expectVector(doc, "data packet header", packet, BleCodec::kLogHeaderBytes,
               GOLDEN_DATA_HEADER);

  length = BleCodec::buildLogFragmentPacket(0, 1, wire, record.sequence, 0, 20,
                                            packet);
  expectVector(doc, "fragment packet", packet, length, GOLDEN_FRAGMENT);

  length = BleCodec::buildLogEndPacket(1, 1202, packet);
  expectVector(doc, "end packet", packet, length, GOLDEN_END);

  uint8_t challenge[BleCodec::kAuthChallengeBytes];
  uint8_t key[BleCodec::kAuthKeyBytes];
  for (uint8_t i = 0; i < 16; i++) {
    challenge[i] = i;
    key[i] = static_cast<uint8_t>(0xa0 + i);
  }
  BleCodec::AuthState authState;
  authState.authenticated = false;
  authState.enrolmentOpen = true;
  authState.enrolled = true;
  authState.challenge = challenge;
  uint8_t auth[BleCodec::kAuthStateBytes];
  BleCodec::encodeAuthState(authState, auth);
  expectVector(doc, "auth state", auth, sizeof auth, GOLDEN_AUTH_STATE);

  uint8_t enrolKey[BleCodec::kEnrolKeyBytes];
  BleCodec::encodeEnrolKey(0x1234, nullptr, enrolKey);
  expectVector(doc, "enrolment key", enrolKey, sizeof enrolKey,
               GOLDEN_ENROL_KEY);
  BleCodec::encodeEnrolKey(0x1234, key, enrolKey);
  expectVector(doc, "issued key", enrolKey, sizeof enrolKey,
               GOLDEN_ISSUED_KEY);
  uint8_t setup[BleCodec::kAuthKeyBytes];
  CHECK(BleCodec::setupKey(123456, 0x1234, setup));
  uint8_t setupProve[BleCodec::kAuthProveBytes] = {BleCodec::kAuthOpProve, 0};
  LittleEndian::putU16(setupProve + 2, 0x1234);
  CHECK(BleCodec::authProof(setup, challenge, 0x1234, setupProve + 4));
  expectVector(doc, "setup prove write", setupProve, sizeof setupProve,
               GOLDEN_SETUP_PROVE);

  const uint8_t enrolWrite[] = {BleCodec::kAuthOpEnrol, 0};
  expectVector(doc, "enrol write", enrolWrite, sizeof enrolWrite,
               GOLDEN_ENROL_WRITE);
  BleCodec::AuthRequest authRequest;
  CHECK(BleCodec::decodeAuthWrite(enrolWrite, sizeof enrolWrite, authRequest));
  CHECK_EQ(uint8_t{BleCodec::kAuthOpEnrol}, authRequest.opcode);

  uint8_t prove[BleCodec::kAuthProveBytes] = {BleCodec::kAuthOpProve, 0};
  LittleEndian::putU16(prove + 2, 0x1234);
  CHECK(BleCodec::authProof(key, challenge, 0x1234, prove + 4));
  expectVector(doc, "prove write", prove, sizeof prove, GOLDEN_AUTH_PROVE);
  CHECK(BleCodec::decodeAuthWrite(prove, sizeof prove, authRequest));
  CHECK_EQ(uint8_t{BleCodec::kAuthOpProve}, authRequest.opcode);
  CHECK_EQ(uint16_t{0x1234}, authRequest.keyId);
  CHECK(memcmp(authRequest.proof, prove + 4, 16) == 0);
  // Wrong lengths and reserved bytes are refused.
  CHECK(!BleCodec::decodeAuthWrite(prove, sizeof prove - 1, authRequest));
  prove[1] = 1;
  CHECK(!BleCodec::decodeAuthWrite(prove, sizeof prove, authRequest));
}

TEST_CASE("HMAC-SHA-256") {
  // RFC 4231 test cases 1, 2 and 6 (long key), checking the wiring to
  // Mbed TLS: the multi-part concatenation and the digest size.
  uint8_t digest[HmacSha256::kMacBytes];
  uint8_t key1[20];
  memset(key1, 0x0b, sizeof key1);
  const char hiThere[] = "Hi There";
  CHECK(HmacSha256::compute(key1, sizeof key1,
                            reinterpret_cast<const uint8_t*>(hiThere), 8,
                            nullptr, 0, nullptr, 0, digest));
  CHECK(toHex(digest, 32) ==
        "b0 34 4c 61 d8 db 38 53 5c a8 af ce af 0b f1 2b 88 1d c2 00 c9 "
        "83 3d a7 26 e9 37 6c 2e 32 cf f7");

  // Test case 2 split across the three message parts.
  const char jefe[] = "Jefe";
  const char part1[] = "what do ya";
  const char part2[] = " want ";
  const char part3[] = "for nothing?";
  CHECK(HmacSha256::compute(
      reinterpret_cast<const uint8_t*>(jefe), 4,
      reinterpret_cast<const uint8_t*>(part1), strlen(part1),
      reinterpret_cast<const uint8_t*>(part2), strlen(part2),
      reinterpret_cast<const uint8_t*>(part3), strlen(part3), digest));
  CHECK(toHex(digest, 32) ==
        "5b dc c1 46 bf 60 75 4e 6a 04 24 26 08 95 75 c7 5a 00 3f 08 9d "
        "27 39 83 9d ec 58 b9 64 ec 38 43");

  uint8_t longKey[131];
  memset(longKey, 0xaa, sizeof longKey);
  const char bigKey[] = "Test Using Larger Than Block-Size Key - Hash Key First";
  CHECK(HmacSha256::compute(longKey, sizeof longKey,
                            reinterpret_cast<const uint8_t*>(bigKey),
                            strlen(bigKey), nullptr, 0, nullptr, 0,
                            digest));
  CHECK(toHex(digest, 32) ==
        "60 e4 31 59 1e e0 b6 7f 0d 8a 26 aa cb f5 b7 7f 8e 0b c6 21 37 "
        "28 c5 14 05 46 04 0f 0e e3 7f 54");

  CHECK(HmacSha256::equal(digest, digest, 32));
  uint8_t other[32];
  memcpy(other, digest, 32);
  other[31] ^= 1;
  CHECK(!HmacSha256::equal(digest, other, 32));
}

TEST_CASE("sleep window validation") {
  const SleepWindow kept = defaultSleepWindow();
  uint8_t good[12];
  encodeSleepWindow(kept, good);
  uint8_t wire[13];
  SleepWindow window = kept;
  auto rejects = [&](const char* why) {
    CAPTURE(why);
    window = kept;
    window.weekdayBedMin = 1;  // a sentinel the decoder must not overwrite
    CHECK(!BleCodec::decodeSleepWindow(wire, 12, window));
    CHECK_EQ(uint16_t{1}, window.weekdayBedMin);
    memcpy(wire, good, 12);
  };
  memcpy(wire, good, 12);
  CHECK(BleCodec::decodeSleepWindow(wire, 12, window));
  CHECK(!BleCodec::decodeSleepWindow(wire, 11, window));
  CHECK(!BleCodec::decodeSleepWindow(wire, 13, window));  // ArduinoBLE spare
  wire[0] = 0x02;
  rejects("reserved flag bit");
  wire[1] = 1;
  rejects("reserved byte");
  LittleEndian::putI16(wire + 2, -721);
  rejects("offset below -720");
  LittleEndian::putI16(wire + 2, 841);
  rejects("offset above +840");
  LittleEndian::putU16(wire + 4, 1440);
  rejects("weekday bedtime");
  LittleEndian::putU16(wire + 6, 1440);
  rejects("weekday wake");
  LittleEndian::putU16(wire + 8, 60);
  rejects("weekend bedtime without wake");
  LittleEndian::putU16(wire + 10, 600);
  rejects("weekend wake without bedtime");
  LittleEndian::putU16(wire + 8, 1440);
  LittleEndian::putU16(wire + 10, 600);
  rejects("weekend bedtime out of range");

  // Edges are accepted.
  LittleEndian::putI16(wire + 2, -720);
  LittleEndian::putU16(wire + 4, 0);
  LittleEndian::putU16(wire + 6, 1439);
  LittleEndian::putU16(wire + 8, 1439);
  LittleEndian::putU16(wire + 10, 0);
  CHECK(BleCodec::decodeSleepWindow(wire, 12, window));
  CHECK(window.hasWeekend());
  CHECK_EQ(int16_t{-720}, window.utcOffsetMinutes);
  LittleEndian::putI16(wire + 2, 840);
  CHECK(BleCodec::decodeSleepWindow(wire, 12, window));
}

TEST_CASE("sleep window persistence") {
  FlashSim::reset();
  W25Q64Flash flash;
  flash.begin();
  ConfigStore store;
  SleepWindow window = {};
  CHECK(!store.loadSleepWindow(flash, window));  // none stored: the default
  CHECK(store.healthy());
  uint8_t wire[12];
  encodeSleepWindow(window, wire);
  CHECK_EQ(std::string(GOLDEN_SLEEP_WINDOW_DEFAULT), toHex(wire, 12));

  // Stored beside the config without touching it.
  CHECK(store.save(flash, sampleConfig()));
  SleepWindow saved = defaultSleepWindow();
  saved.flags = SleepWindow::kFlagFollowApp;
  saved.utcOffsetMinutes = -300;
  saved.weekendBedMin = 60;
  saved.weekendWakeMin = 600;
  CHECK(store.saveSleepWindow(flash, saved));
  SleepWindow invalid = saved;
  invalid.weekdayWakeMin = 1440;
  CHECK(!store.saveSleepWindow(flash, invalid));
  ConfigStore rebooted;
  Config config = defaultConfig();
  CHECK(rebooted.load(flash, config));
  CHECK_EQ(sampleConfig().measurementIntervalSeconds,
           config.measurementIntervalSeconds);
  SleepWindow loaded = defaultSleepWindow();
  CHECK(rebooted.loadSleepWindow(flash, loaded));
  CHECK(loaded.followsApp());
  CHECK_EQ(int16_t{-300}, loaded.utcOffsetMinutes);
  CHECK_EQ(uint16_t{60}, loaded.weekendBedMin);
  CHECK_EQ(uint16_t{600}, loaded.weekendWakeMin);

  // An interrupted update keeps the committed window.
  SleepWindow next = loaded;
  next.utcOffsetMinutes = -240;
  FlashSim::failNextPrograms(1);
  CHECK(!rebooted.saveSleepWindow(flash, next));
  ConfigStore recovered;
  CHECK(recovered.loadSleepWindow(flash, loaded));
  CHECK_EQ(int16_t{-300}, loaded.utcOffsetMinutes);
}

}  // namespace

