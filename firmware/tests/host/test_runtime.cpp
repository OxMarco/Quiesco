// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cstring>
#include <vector>
#include "doctest.h"
#include <Arduino.h>
#include <ArduinoBLE.h>
#include <Wire.h>
#include <utility/HCI.h>
#include "App.h"
#include "diagnostics/DebugLog.h"
#include "platform/BootloaderEntry.h"
#include "platform/DeviceId.h"
#include "platform/ResetReason.h"
#include "protocol/LittleEndian.h"
#include "storage/FlashStorageLayout.h"
#include "storage/TraceStore.h"
#include "ui/Renderer.h"
#include "W25Q64FlashSim.h"

BLEClass BLE;
HCIClass HCI;
TwoWire Wire;
bool sensorReady = true;
int otaEntries = 0;
int usbUpdateEntries = 0;
bool onUsb = false;
float batteryVolts = 3.8f;
namespace {
uint32_t ticks = 0;
std::function<void()> rngEvent;
constexpr char kAuth[] = "7A1E0010-8E6F-4A7A-AE32-515549455343";
constexpr char kInterval[] = "7A1E0001-8E6F-4A7A-AE32-515549455343";
constexpr char kReset[] = "7A1E000D-8E6F-4A7A-AE32-515549455343";
constexpr char kReading[] = "7A1E0003-8E6F-4A7A-AE32-515549455343";
constexpr char kStatus[] = "7A1E0004-8E6F-4A7A-AE32-515549455343";
constexpr char kSleep[] = "7A1E0012-8E6F-4A7A-AE32-515549455343";
const uint8_t key[16] = {1, 2, 3};
BondTable knownBonds() {
  BondTable bonds;
  const uint8_t address[6] = {0x34, 0x12};
  bonds.storeLtk(address, key);
  return bonds;
}
void prove() {
  const auto state = FakeBle::read(kAuth);
  uint8_t proof[20] = {2, 0, 0x34, 0x12};
  CHECK_MESSAGE(BleCodec::authProof(key, state.data() + 2, 0x1234, proof + 4), "proof creation");
  FakeBle::write(kAuth, proof, sizeof proof);
}
void step(App& app, uint32_t elapsed = 10) { ticks += elapsed; app.step(); }
void run(App& app, int steps) { for (int i = 0; i < steps; ++i) step(app); }
TEST_CASE("reconnect") {
  BleConfig adapter;
  adapter.setBonds(knownBonds());
  CHECK_MESSAGE(adapter.begin(defaultConfig(), "0123456789abcdef", ticks), "BLE begin");
  FakeBle::connect(1); adapter.poll(ticks);
  prove(); adapter.poll(ticks);
  CHECK_MESSAGE(adapter.authenticated(), "known phone authenticates");
  Reading reading; reading.temperatureC = 23; reading.valid = VALID_TEMPERATURE;
  adapter.publishReading(reading);
  // One HCI poll drains disconnect, reconnect, a read and a write.
  FakeBle::events().push_back([&]() {
    FakeBle::disconnect(); FakeBle::connect(2);
    const auto bytes = FakeBle::read(kReading);
    CHECK_MESSAGE(std::all_of(bytes.begin(), bytes.end(), [](uint8_t b) { return b == 0; }),
                  "new peer must see zeros before poll returns");
    const uint8_t interval[4] = {60, 0, 0, 0};
    FakeBle::write(kInterval, interval, sizeof interval);
  });
  adapter.poll(ticks);
  Config changed;
  CHECK_MESSAGE(!adapter.authenticated(), "replacement peer must not inherit authorization");
  CHECK_MESSAGE(!adapter.takePendingConfig(changed), "replacement peer write must be rejected");
  // A write authorized on the old link cannot be replayed by the new one.
  prove(); adapter.poll(ticks);
  const uint8_t interval[4] = {60, 0, 0, 0};
  FakeBle::write(kInterval, interval, sizeof interval);
  FakeBle::disconnect(); FakeBle::connect(3); adapter.poll(ticks);
  CHECK_MESSAGE(!adapter.takePendingConfig(changed), "discard previous session's pending writes");
  // More than 256 failed proofs must not wrap the five-attempt limit.
  uint8_t wrong[20] = {2, 0, 0x34, 0x12};
  for (int i = 0; i < 260; ++i) {
    FakeBle::write(kAuth, wrong, sizeof wrong); adapter.poll(ticks);
  }
  prove(); adapter.poll(ticks);
  CHECK_MESSAGE(!adapter.authenticated(), "failed proof limit must saturate");
  FakeBle::disconnect();
  FakeBle::connect(6); adapter.poll(ticks);
  prove();
  rngEvent = [] { FakeBle::disconnect(); FakeBle::connect(7); };
  adapter.poll(ticks);
  CHECK_MESSAGE(!adapter.authenticated(), "connection change during HCI RNG must invalidate accepted proof");
  FakeBle::disconnect();
}
TEST_CASE("enrollment") {
  constexpr char metadata[] = "7A1E0011-8E6F-4A7A-AE32-515549455343";
  const uint8_t enrol[2] = {1, 0};
  BleConfig adapter;
  BondTable empty; adapter.setBonds(empty);
  adapter.begin(defaultConfig(), "0123456789abcdef", 0);
  adapter.setPairingAllowed(false);
  FakeBle::connect(10); adapter.poll(0);
  FakeBle::write(kAuth, enrol, sizeof enrol); adapter.poll(1);
  CHECK_MESSAGE(!adapter.enrollmentPending(), "enrollment requires USB");
  adapter.setPairingAllowed(true);
  FakeBle::write(kAuth, enrol, sizeof enrol); adapter.poll(2);
  CHECK_MESSAGE(adapter.enrollmentPending(), "USB allows a pending on-screen key");
  CHECK_MESSAGE(adapter.bondCount() == 0, "unauthorized ENROL must not save or evict keys");
  uint32_t code = 0;
  CHECK_MESSAGE((adapter.takeEnrollment(code) && code < BleCodec::kSetupCodeCount),
                "local display gets a six-digit code");
  auto wire = FakeBle::read(metadata);
  uint16_t keyId = LittleEndian::getU16(wire.data());
  CHECK_MESSAGE(keyId != 0, "radio exposes public key id");
  CHECK_MESSAGE(std::all_of(wire.begin() + 2, wire.end(), [](uint8_t b) { return b == 0; }),
                "no key appears in enrollment characteristic before the code is proved");
  // Repeated ENROL does not replace the code being typed or extend its lifetime.
  FakeBle::write(kAuth, enrol, sizeof enrol); adapter.poll(1000);
  CHECK_MESSAGE(!adapter.takeEnrollment(code), "ENROL retries keep the current screen");
  uint8_t setup[16];
  CHECK_MESSAGE(BleCodec::setupKey(code, keyId, setup), "derive setup key");
  auto state = FakeBle::read(kAuth);
  uint8_t proof[20] = {2, 0}; LittleEndian::putU16(proof + 2, keyId);
  // A wrong code is refused and leaves setup pending.
  uint8_t wrongSetup[16];
  BleCodec::setupKey((code + 1) % BleCodec::kSetupCodeCount, keyId, wrongSetup);
  BleCodec::authProof(wrongSetup, state.data() + 2, keyId, proof + 4);
  FakeBle::write(kAuth, proof, sizeof proof); adapter.poll(1001);
  CHECK_MESSAGE((!adapter.authenticated() && adapter.bondCount() == 0), "wrong code must not enroll");
  state = FakeBle::read(kAuth);
  BleCodec::authProof(setup, state.data() + 2, keyId, proof + 4);
  FakeBle::write(kAuth, proof, sizeof proof); adapter.poll(1002);
  CHECK_MESSAGE((adapter.authenticated() && adapter.bondCount() == 1), "only proof of display code enrolls");
  CHECK_MESSAGE(!adapter.enrollmentPending(), "successful proof scrubs pending setup");
  // The phone is handed a fresh full-length key, not one derived from the code.
  wire = FakeBle::read(metadata);
  CHECK_MESSAGE(LittleEndian::getU16(wire.data()) == keyId, "issued key carries its id");
  uint8_t issued[16];
  std::copy(wire.begin() + 4, wire.end(), issued);
  CHECK_MESSAGE((std::any_of(issued, issued + 16, [](uint8_t b) { return b != 0; }) &&
                     !std::equal(issued, issued + 16, setup)),
                "issued key is random, not the setup key");
  adapter.poll(1002 + 30000);
  wire = FakeBle::read(metadata);
  CHECK_MESSAGE(std::all_of(wire.begin(), wire.end(), [](uint8_t b) { return b == 0; }),
                "issued key is withdrawn after 30 s");
  FakeBle::disconnect(); FakeBle::connect(12); adapter.poll(40000);
  state = FakeBle::read(kAuth);
  BleCodec::authProof(setup, state.data() + 2, keyId, proof + 4);
  FakeBle::write(kAuth, proof, sizeof proof); adapter.poll(40001);
  CHECK_MESSAGE(!adapter.authenticated(), "the setup key is not a phone key");
  state = FakeBle::read(kAuth);
  BleCodec::authProof(issued, state.data() + 2, keyId, proof + 4);
  FakeBle::write(kAuth, proof, sizeof proof); adapter.poll(40002);
  CHECK_MESSAGE(adapter.authenticated(), "the issued key signs in");
  FakeBle::disconnect(); FakeBle::connect(11); adapter.poll(42000);
  FakeBle::write(kAuth, enrol, sizeof enrol); adapter.poll(42001);
  CHECK_MESSAGE(adapter.takeEnrollment(code), "second enrollment can start");
  keyId = LittleEndian::getU16(FakeBle::read(metadata).data());
  BleCodec::setupKey(code, keyId, setup);
  adapter.poll(222002);
  state = FakeBle::read(kAuth);
  LittleEndian::putU16(proof + 2, keyId);
  BleCodec::authProof(setup, state.data() + 2, keyId, proof + 4);
  FakeBle::write(kAuth, proof, sizeof proof); adapter.poll(222003);
  CHECK_MESSAGE((!adapter.authenticated() && adapter.bondCount() == 1), "expired setup code cannot enroll");
  FakeBle::write(kAuth, enrol, sizeof enrol); adapter.poll(222004);
  adapter.setPairingAllowed(false);
  CHECK_MESSAGE(!adapter.enrollmentPending(), "unplugging cancels setup immediately");
  FakeBle::disconnect();
}
TEST_CASE("FRC retry") {
  Scd41Sensor sensor;
  CHECK_MESSAGE(sensor.begin(), "SCD begin");
  sensor.startFrcRunUp(0); sensor.pollFrcRunUp(0);
  sensorReady = false;
  CHECK_MESSAGE(sensor.pollFrcRunUp(5000) == Scd41Sensor::FrcProgress::kRunning,
                "not-ready at five seconds must be retried");
  sensorReady = true;
  CHECK_MESSAGE(sensor.pollFrcRunUp(5100) == Scd41Sensor::FrcProgress::kRunning,
                "slightly late valid measurement must continue run-up");
  for (uint64_t shot = 1; shot < 5; ++shot) {
    sensor.pollFrcRunUp(shot * 60000);
    sensor.pollFrcRunUp(shot * 60000 + 5000);
  }
  CHECK_MESSAGE(sensor.pollFrcRunUp(300000) == Scd41Sensor::FrcProgress::kReady,
                "all five shots complete preparation");
  int16_t correction = 0;
  CHECK_MESSAGE(sensor.performForcedRecalibration(450, correction), "prepared FRC succeeds");
  CHECK_MESSAGE(!sensor.performForcedRecalibration(450, correction), "preparation used once");
  sensor.startFrcRunUp(0); sensor.pollFrcRunUp(0); sensorReady = false;
  CHECK_MESSAGE(sensor.pollFrcRunUp(7000) == Scd41Sensor::FrcProgress::kFailed,
                "stuck not-ready must fail at deadline");
  sensorReady = true;
}
TEST_CASE("app erase recovery") {
  FlashSim::reset(); ticks = 0;
  W25Q64Flash flash; flash.begin();
  ConfigStore config;
  CHECK_MESSAGE(config.save(flash, defaultConfig()), "save config");
  CHECK_MESSAGE(config.saveBonds(flash, knownBonds()), "save phone key");
  SampleLog original;
  Reading reading;
  for (int i = 0; i < 200; ++i) CHECK_MESSAGE(original.append(flash, reading), "seed log");
  {
    App app; app.begin(); step(app, 1001); run(app, 20);
    FakeBle::connect(4); step(app); prove(); step(app);
    const uint8_t reset[4] = {1, 1, 0xC7, 0xFA};
    FakeBle::write(kReset, reset, sizeof reset);
    const auto before = FlashSim::sampleSectorErases();
    for (int i = 0; i < 250 && FlashSim::sampleSectorErases() == before; ++i) step(app);
    CHECK_MESSAGE(FlashSim::sampleSectorErases() > before, "factory reset starts erase");
    ConfigStore journal; uint32_t floor = 0;
    CHECK_MESSAGE((journal.loadLogErase(flash, floor) && floor >= 202), "intent durable before first erase");
    // Destroy the application while almost all old sectors remain.
    FakeBle::disconnect();
  }
  {
    App reboot; reboot.begin(); step(reboot, 1001);
    const uint32_t sectors = FlashStorageLayout::kSampleDbBytes / FlashStorageLayout::kSectorBytes;
    run(reboot, sectors + 40);
    ConfigStore journal; uint32_t floor = 99;
    CHECK_MESSAGE((journal.loadLogErase(flash, floor) && floor == 0), "reboot finishes deletion and clears journal");
    SampleLog recovered; SampleLogCursor cursor; SampleRecord record;
    CHECK_MESSAGE(recovered.startRead(flash, 0, cursor), "new log readable");
    CHECK_MESSAGE((recovered.readNext(flash, cursor, record) && record.sequence >= 202),
                  "new sequence cannot reuse pre-reset history");
    CHECK_MESSAGE(!recovered.readNext(flash, cursor, record), "no old records survive resumed erase");
  }
}
TEST_CASE("app keeps erase intent until append") {
  FlashSim::reset(); ticks = 0;
  W25Q64Flash flash; flash.begin(); ConfigStore store;
  CHECK_MESSAGE(store.save(flash, defaultConfig()), "seed config for final-erase interruption");
  CHECK_MESSAGE(store.saveLogErase(flash, 42), "seed pending erase floor");
  const uint32_t sectors = FlashStorageLayout::kSampleDbBytes / FlashStorageLayout::kSectorBytes;
  {
    App app; app.begin(); step(app, 1001);
    for (uint32_t i = 0; i < sectors + 30 && FlashSim::sampleSectorErases() < sectors - 1; ++i) step(app);
    CHECK_MESSAGE(FlashSim::sampleSectorErases() == sectors - 1, "stop before final sector");
    FlashSim::failNextPrograms(1);  // first fresh append/format cannot commit
    step(app);
    ConfigStore check; uint32_t floor = 0;
    CHECK_MESSAGE((check.loadLogErase(flash, floor) && floor == 42),
                  "failed first append must retain durable erase intent");
  }
  App reboot; reboot.begin(); step(reboot, 1001); run(reboot, sectors + 40);
  ConfigStore check; uint32_t floor = 0;
  CHECK_MESSAGE((check.loadLogErase(flash, floor) && floor == 0), "healthy reboot finishes interrupted reset");
  SampleLog log; SampleLogCursor cursor; SampleRecord record;
  CHECK_MESSAGE((log.startRead(flash, 0, cursor) && log.readNext(flash, cursor, record) && record.sequence == 42),
                "sequence floor survives a completely erased partition and reboot");
}
uint32_t newestSequence() { return LittleEndian::getU32(FakeBle::read(kStatus).data() + 12); }
bool eraseFlag() { return (FakeBle::read(kStatus)[16] & 4) != 0; }
void writeErase(uint32_t upTo) {
  uint8_t erase[8] = {2, 0, 0xC7, 0xFA};
  LittleEndian::putU32(erase + 4, upTo);
  FakeBle::write(kReset, erase, sizeof erase);
}
TEST_CASE("app erase command") {
  FlashSim::reset(); ticks = 0;
  W25Q64Flash flash; flash.begin(); ConfigStore store;
  Config saved = defaultConfig(); saved.measurementIntervalSeconds = 60;
  CHECK_MESSAGE(store.save(flash, saved), "seed config");
  CHECK_MESSAGE(store.saveBonds(flash, knownBonds()), "seed phone key");
  SleepWindow window = defaultSleepWindow(); window.flags = 1; window.utcOffsetMinutes = 120;
  CHECK_MESSAGE(store.saveSleepWindow(flash, window), "seed sleep window");
  SampleLog original; Reading reading;
  for (int i = 0; i < 50; ++i) CHECK_MESSAGE(original.append(flash, reading), "seed log");
  App app; app.begin(); step(app, 1001); run(app, 20);
  FakeBle::connect(20); step(app); prove(); step(app);
  const uint32_t newest = newestSequence();
  CHECK_MESSAGE(newest == 51, "first cycle appended after the seeded records");
  const auto before = FlashSim::sampleSectorErases();

  // A record later than up-to: refused, nothing erased, bit 2 never set.
  writeErase(newest - 1);
  for (int i = 0; i < 60; ++i) {
    step(app);
    CHECK_MESSAGE(!eraseFlag(), "refused erase never shows as pending");
  }
  CHECK_MESSAGE(FlashSim::sampleSectorErases() == before, "refused erase erases nothing");
  ConfigStore journal; uint32_t floor = 99;
  CHECK_MESSAGE((journal.loadLogErase(flash, floor) && floor == 0), "refused erase journals nothing");
  // Wrong lengths and the factory reset's 4-byte form with opcode 2 are ignored.
  const uint8_t shortErase[4] = {2, 0, 0xC7, 0xFA};
  FakeBle::write(kReset, shortErase, sizeof shortErase);
  uint8_t longErase[9] = {2, 0, 0xC7, 0xFA, 0xFF, 0xFF, 0xFF, 0xFF, 0};
  FakeBle::write(kReset, longErase, sizeof longErase);
  run(app, 30);
  CHECK_MESSAGE((!eraseFlag() && FlashSim::sampleSectorErases() == before), "malformed erase ignored");

  // Up to the newest record: accepted at once, then the journalled erase.
  const uint32_t current = newestSequence();
  writeErase(current);
  step(app);
  CHECK_MESSAGE(eraseFlag(), "accepted erase sets status bit 2 at once");
  for (int i = 0; i < 250 && FlashSim::sampleSectorErases() == before; ++i) step(app);
  CHECK_MESSAGE(FlashSim::sampleSectorErases() > before, "erase command starts the erase");
  ConfigStore started;
  CHECK_MESSAGE((started.loadLogErase(flash, floor) && floor == current + 1), "intent and floor durable first");
  const uint32_t sectors = FlashStorageLayout::kSampleDbBytes / FlashStorageLayout::kSectorBytes;
  run(app, sectors + 40);
  CHECK_MESSAGE(!eraseFlag(), "bit 2 clears when the erase is done");
  ConfigStore finished;
  CHECK_MESSAGE((finished.loadLogErase(flash, floor) && floor == 0), "journal cleared after the fresh record");
  CHECK_MESSAGE(newestSequence() == current + 1, "sequences continue");
  SampleLog log; SampleLogCursor cursor; SampleRecord record;
  CHECK_MESSAGE((log.startRead(flash, 0, cursor) && log.readNext(flash, cursor, record) &&
                 record.sequence == current + 1 && !log.readNext(flash, cursor, record)),
                "only the fresh record remains");
  // Nothing else changed: config, phone keys, sleep window.
  ConfigStore check; Config config; BondTable bonds; SleepWindow kept;
  CHECK_MESSAGE((check.load(flash, config) && config.measurementIntervalSeconds == 60), "config kept");
  CHECK_MESSAGE((check.loadBonds(flash, bonds) && bonds.count() == 1), "phone keys kept");
  CHECK_MESSAGE((check.loadSleepWindow(flash, kept) && kept.utcOffsetMinutes == 120), "sleep window kept");
  FakeBle::disconnect();
}
TEST_CASE("app erase command before the log is mounted") {
  FlashSim::reset(); ticks = 0;
  W25Q64Flash flash; flash.begin(); ConfigStore store;
  CHECK_MESSAGE(store.save(flash, defaultConfig()), "seed config");
  CHECK_MESSAGE(store.saveBonds(flash, knownBonds()), "seed phone key");
  SampleLog original; Reading reading;
  for (int i = 0; i < 30; ++i) CHECK_MESSAGE(original.append(flash, reading), "seed log");
  const auto before = FlashSim::sampleSectorErases();
  // Authenticate between loading settings and the first append: the app
  // only knows "up to 0", which the 30 records on flash must refuse.
  App app; app.begin(); step(app, 1001); step(app);  // settings loaded, BLE up
  FakeBle::connect(21); step(app); prove(); step(app);
  writeErase(0);  // reaches App before this cycle's first append
  run(app, 80);
  CHECK_MESSAGE(FlashSim::sampleSectorErases() == before, "unmounted log is checked before erasing");
  CHECK_MESSAGE(!eraseFlag(), "refused erase leaves bit 2 clear");
  CHECK_MESSAGE(newestSequence() >= 31, "history kept");
  FakeBle::disconnect();
}
TEST_CASE("app enters the OTA bootloader") {
  FlashSim::reset(); ticks = 0; otaEntries = 0;
  W25Q64Flash flash; flash.begin(); ConfigStore store;
  CHECK_MESSAGE(store.save(flash, defaultConfig()), "seed config");
  CHECK_MESSAGE(store.saveBonds(flash, knownBonds()), "seed phone key");
  App app; app.begin(); step(app, 1001); run(app, 40);  // first cycle done
  const uint8_t update[4] = {3, 0, 0xC7, 0xFA};

  // Unauthenticated, malformed or on a low battery: nothing happens.
  FakeBle::connect(22); step(app);
  FakeBle::write(kReset, update, sizeof update); run(app, 60);
  CHECK_MESSAGE(otaEntries == 0, "unauthenticated update ignored");
  prove(); step(app);
  const uint8_t flagged[4] = {3, 1, 0xC7, 0xFA};
  const uint8_t unconfirmed[4] = {3, 0, 0xC7, 0xFB};
  const uint8_t longer[8] = {3, 0, 0xC7, 0xFA};
  FakeBle::write(kReset, flagged, sizeof flagged); run(app, 20);
  FakeBle::write(kReset, unconfirmed, sizeof unconfirmed); run(app, 20);
  FakeBle::write(kReset, longer, sizeof longer); run(app, 60);
  CHECK_MESSAGE(otaEntries == 0, "malformed update ignored");
  batteryVolts = 3.5f;
  step(app, 300000);
  for (int i = 0; i < 300; ++i) step(app, 100);  // a measurement reads it
  FakeBle::write(kReset, update, sizeof update); run(app, 100);
  CHECK_MESSAGE(otaEntries == 0, "update refused on a low battery");
  batteryVolts = 3.8f;
  step(app, 300000);
  for (int i = 0; i < 300; ++i) step(app, 100);

  // Accepted: the link drops, then the reset follows once idle.
  FakeBle::write(kReset, update, sizeof update);
  step(app);
  CHECK_MESSAGE(!bool(FakeBle::peer()), "phone disconnected before the reset");
  CHECK_MESSAGE(otaEntries == 0, "reset waits for the disconnect to go out");
  step(app, 400);
  CHECK_MESSAGE(otaEntries == 1, "bootloader entered");
}
TEST_CASE("app enters the USB update drive") {
  FlashSim::reset(); ticks = 0; otaEntries = 0; usbUpdateEntries = 0; onUsb = false;
  W25Q64Flash flash; flash.begin(); ConfigStore store;
  CHECK_MESSAGE(store.save(flash, defaultConfig()), "seed config");
  CHECK_MESSAGE(store.saveBonds(flash, knownBonds()), "seed phone key");
  App app; app.begin(); step(app, 1001); run(app, 40);
  FakeBle::connect(23); step(app); prove(); step(app);
  const uint8_t usbUpdate[4] = {4, 0, 0xC7, 0xFA};
  FakeBle::write(kReset, usbUpdate, sizeof usbUpdate); run(app, 100);
  CHECK_MESSAGE(usbUpdateEntries == 0, "refused on battery: no drive to show");
  onUsb = true;
  FakeBle::write(kReset, usbUpdate, sizeof usbUpdate);
  step(app);
  CHECK_MESSAGE(!bool(FakeBle::peer()), "phone disconnected before the reset");
  step(app, 400);
  CHECK_MESSAGE((usbUpdateEntries == 1 && otaEntries == 0), "UF2 bootloader entered");
  onUsb = false;
}
TEST_CASE("app sleep window") {
  FlashSim::reset(); ticks = 0;
  W25Q64Flash flash; flash.begin(); ConfigStore store;
  CHECK_MESSAGE(store.save(flash, defaultConfig()), "seed config");
  CHECK_MESSAGE(store.saveBonds(flash, knownBonds()), "seed phone key");
  const std::vector<uint8_t> defaults = {0, 0, 0, 0, 0x82, 0x05, 0xA4, 0x01, 0xFF, 0xFF, 0xFF, 0xFF};
  const std::vector<uint8_t> golden = {1, 0, 0x78, 0, 0x82, 0x05, 0xA4, 0x01, 0xFF, 0xFF, 0xFF, 0xFF};
  {
    App app; app.begin(); step(app, 1001); run(app, 20);
    FakeBle::connect(22); step(app); prove(); step(app);
    CHECK_MESSAGE(FakeBle::read(kSleep) == defaults, "fresh unit reads the default window");
    FakeBle::write(kSleep, golden.data(), static_cast<int>(golden.size()));
    run(app, 40);
    CHECK_MESSAGE(FakeBle::read(kSleep) == golden, "valid write applied");
    std::vector<uint8_t> bad = golden; bad[10] = 0x00; bad[11] = 0x01;  // only one weekend 0xFFFF
    FakeBle::write(kSleep, bad.data(), static_cast<int>(bad.size()));
    step(app);
    CHECK_MESSAGE(FakeBle::read(kSleep) == golden, "invalid write reverts");
    bad = golden; bad.push_back(0);  // 13 bytes
    FakeBle::write(kSleep, bad.data(), static_cast<int>(bad.size()));
    step(app);
    CHECK_MESSAGE(FakeBle::read(kSleep) == golden, "over-long write reverts");
    ConfigStore check; SleepWindow stored;
    CHECK_MESSAGE((check.loadSleepWindow(flash, stored) && stored.followsApp() &&
                   stored.utcOffsetMinutes == 120), "window saved to flash");
    FakeBle::disconnect();
  }
  {
    App reboot; reboot.begin(); step(reboot, 1001); run(reboot, 20);
    FakeBle::connect(23); step(reboot); prove(); step(reboot);
    CHECK_MESSAGE(FakeBle::read(kSleep) == golden, "window survives a reboot");
    const uint8_t reset[4] = {1, 0, 0xC7, 0xFA};
    FakeBle::write(kReset, reset, sizeof reset);
    run(reboot, 60);
    CHECK_MESSAGE(FakeBle::read(kSleep) == defaults, "factory reset restores the default window");
    ConfigStore check; SleepWindow stored;
    CHECK_MESSAGE((check.loadSleepWindow(flash, stored) && !stored.followsApp() &&
                   stored.utcOffsetMinutes == 0), "default window saved by the reset");
    FakeBle::disconnect();
  }
}
TEST_CASE("app retries config read") {
  FlashSim::reset(); ticks = 0;
  W25Q64Flash flash; flash.begin(); ConfigStore store;
  Config saved = defaultConfig(); saved.measurementIntervalSeconds = 60;
  CHECK_MESSAGE(store.save(flash, saved), "seed settings");
  CHECK_MESSAGE(store.saveBonds(flash, knownBonds()), "seed bonds");
  FlashSim::failReadAt(1);
  App app; app.begin(); step(app, 1001); run(app, 20);
  ConfigStore intact; Config loaded;
  CHECK_MESSAGE((intact.load(flash, loaded) && loaded.measurementIntervalSeconds == 60),
                "failed boot read must not replace config with defaults");
  step(app, 300001); step(app, 1001); run(app, 20);
  FakeBle::connect(5); step(app); prove(); step(app);
  const auto interval = FakeBle::read(kInterval);
  CHECK_MESSAGE((interval.size() == 4 && LittleEndian::getU32(interval.data()) == 60),
                "subsequent cycle recovers settings and phone key");
  FakeBle::disconnect();
}
}
uint32_t millis() { return ticks; }
void delay(unsigned long milliseconds) { ticks += milliseconds; }
void digitalWrite(uint8_t, uint8_t) {}
void pinMode(uint8_t, uint8_t) {}
int HCIClass::leRand(uint8_t* bytes) {
  auto event = rngEvent; rngEvent = nullptr;
  if (event) event();
  static uint8_t n = 1;
  for (int i = 0; i < 8; ++i) bytes[i] = n++;
  return 0;
}
void BatteryMonitor::begin() { chargingStable_ = lastRaw_ = primed_ = false; rawSinceMs_ = 0; }
bool BatteryMonitor::sample(float& v) { v = batteryVolts; return true; }
bool BatteryMonitor::pollCharging(uint64_t) { return false; }
bool BatteryMonitor::usbPowered() const { return onUsb; }
bool EnvironmentalSampler::begin() { busStarted_ = true; pressureHandedOff_ = false; return true; }
void EnvironmentalSampler::start(uint64_t) {}
void EnvironmentalSampler::poll(uint64_t) {}
bool EnvironmentalSampler::ready() const { return true; }
void EnvironmentalSampler::collect(Reading& r) { r.temperatureC = 22; r.valid |= VALID_TEMPERATURE; }
void EnvironmentalSampler::end() {}
bool EnvironmentalSampler::bme280Present() const { return true; }
bool EnvironmentalSampler::bme280TimedOut() const { return false; }
bool EnvironmentalSampler::veml7700Present() const { return false; }
bool EnvironmentalSampler::veml7700TimedOut() const { return false; }
bool EnvironmentalSampler::scd41Present() const { return false; }
bool EnvironmentalSampler::scd41TimedOut() const { return false; }
bool PdmMicrophone::begin() { return true; }
void PdmMicrophone::start(uint64_t) {}
void PdmMicrophone::poll(uint64_t) {}
bool PdmMicrophone::ready() const { return true; }
void PdmMicrophone::collect(Reading&) {}
void PdmMicrophone::end() {}
bool PdmMicrophone::present() const { return false; }
bool PdmMicrophone::timedOut() const { return false; }
bool EpaperDisplay::begin(bool) { return true; }
bool EpaperDisplay::end() { return true; }
bool EpaperDisplay::present() const { return true; }
bool EpaperDisplay::timedOut() const { return false; }
namespace Renderer { bool render(EpaperDisplay&, const UiModel&) { return true; } }
void HardwareWatchdog::begin() { active_ = true; }
void HardwareWatchdog::kick() {}
void BootloaderEntry::enterOtaUpdate() { ++otaEntries; }
void BootloaderEntry::enterUsbUpdate() { ++usbUpdateEntries; }
namespace DeviceId { uint64_t read() { return 123; } }
namespace ResetReason { uint32_t take() { return 0; } const char* describe(uint32_t) { return "test"; } }
namespace TraceStore { bool flush(W25Q64Flash&) { return true; } void dump(W25Q64Flash&) {} }
