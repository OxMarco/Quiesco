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
#include "platform/RetainedRam.h"
#include "protocol/LittleEndian.h"
#include "storage/FlashStorageLayout.h"
#include "storage/TraceStore.h"
#include "ui/Renderer.h"
#include "W25Q64FlashSim.h"

BLEClass BLE;
HCIClass HCI;
TwoWire Wire;
bool sensorReady = true;
bool samplerHangs = false;  // a sensor driver stuck without its own timeout
int usbUpdateEntries = 0;
bool onUsb = false;
// Retained RAM and RESETREAS as the next App::begin() sees them.
uint32_t nextResetBits = 0;
ResetHistory::Breadcrumb retainedBreadcrumb;
bool retainedValid = false;
float batteryVolts = 3.8f;
namespace {
uint32_t ticks = 0;
uint32_t lastKickTicks = 0;
std::function<void()> rngEvent;
bool rngFails = false;
constexpr char kAuth[] = "7A1E0010-8E6F-4A7A-AE32-515549455343";
constexpr char kInterval[] = "7A1E0001-8E6F-4A7A-AE32-515549455343";
constexpr char kReset[] = "7A1E000D-8E6F-4A7A-AE32-515549455343";
constexpr char kReading[] = "7A1E0003-8E6F-4A7A-AE32-515549455343";
constexpr char kStatus[] = "7A1E0004-8E6F-4A7A-AE32-515549455343";
constexpr char kSleep[] = "7A1E0012-8E6F-4A7A-AE32-515549455343";
constexpr char kSyncControl[] = "7A1E0009-8E6F-4A7A-AE32-515549455343";
constexpr char kLogData[] = "7A1E000A-8E6F-4A7A-AE32-515549455343";
constexpr char kName[] = "7A1E0008-8E6F-4A7A-AE32-515549455343";
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
  // A second ENROL while the issued key waits to be read would replace it.
  FakeBle::write(kAuth, enrol, sizeof enrol); adapter.poll(1003);
  CHECK_MESSAGE(!adapter.takeEnrollment(code), "no new setup while a key is on offer");
  // Unplugging now keeps the key the unit has already stored.
  adapter.setPairingAllowed(false); adapter.poll(1004);
  adapter.setPairingAllowed(true); adapter.poll(1005);
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
TEST_CASE("a session change inside a notify keeps values hidden") {
  BleConfig adapter;
  adapter.setBonds(knownBonds());
  adapter.begin(defaultConfig(), "0123456789abcdef", ticks);
  Reading reading; reading.temperatureC = 23; reading.valid = VALID_TEMPERATURE;
  adapter.publishReading(reading);
  adapter.publishSleepWindow(defaultSleepWindow());
  FakeBle::connect(70); adapter.poll(ticks);
  // The phone subscribed before proving; exposing the reading notifies, and
  // the link drops and a new phone connects while that notify waits.
  FakeBle::values().at(kReading)->subscribed = true;
  FakeBle::duringNotify() = [] { FakeBle::disconnect(); FakeBle::connect(71); };
  prove(); adapter.poll(ticks);
  CHECK(!adapter.authenticated());
  for (const char* uuid : {kReading, kSleep, kStatus}) {
    CAPTURE(uuid);
    const auto bytes = FakeBle::read(uuid);
    CHECK(std::all_of(bytes.begin(), bytes.end(), [](uint8_t b) { return b == 0; }));
  }
  FakeBle::duringNotify() = nullptr;
  FakeBle::disconnect();
}
TEST_CASE("setup codes are limited per plug-in") {
  const uint8_t enrol[2] = {1, 0};
  BleConfig adapter;
  BondTable empty; adapter.setBonds(empty);
  adapter.begin(defaultConfig(), "0123456789abcdef", 0);
  adapter.setPairingAllowed(true);
  uint32_t code = 0;
  // Each reconnect resets the failure count, but not the codes issued.
  for (int i = 0; i < 10; ++i) {  // kMaxSetupCodesPerPlug
    FakeBle::connect(40 + i); adapter.poll(i * 10);
    FakeBle::write(kAuth, enrol, sizeof enrol); adapter.poll(i * 10 + 1);
    CHECK_MESSAGE(adapter.takeEnrollment(code), "a code is shown");
    FakeBle::disconnect(); adapter.poll(i * 10 + 2);
  }
  FakeBle::connect(60); adapter.poll(1000);
  FakeBle::write(kAuth, enrol, sizeof enrol); adapter.poll(1001);
  CHECK_MESSAGE(!adapter.enrollmentPending(), "no further code until a replug");
  adapter.setPairingAllowed(false); adapter.setPairingAllowed(true);
  FakeBle::write(kAuth, enrol, sizeof enrol); adapter.poll(1002);
  CHECK_MESSAGE(adapter.enrollmentPending(), "a replug allows setup again");
  adapter.setPairingAllowed(false);
  FakeBle::disconnect();
}
TEST_CASE("advertising after a disconnect") {
  Config plugged = defaultConfig();
  plugged.bleAlwaysAvailable = false;
  BleConfig adapter;
  adapter.setBonds(knownBonds());
  adapter.begin(plugged, "0123456789abcdef", 0);
  adapter.applyAvailabilityPolicy(plugged, 0);
  adapter.setPairingAllowed(true);
  adapter.poll(0);
  CHECK_MESSAGE(FakeBle::advertising(), "advertises on USB");
  // Unplugged mid-connection: the stack would resume advertising on battery.
  FakeBle::connect(30); adapter.poll(1);
  adapter.setPairingAllowed(false);
  FakeBle::disconnect(); adapter.poll(2);
  CHECK_MESSAGE(!FakeBle::advertising(), "silent on battery after the phone leaves");
  // A rename during a connection is advertised once the phone leaves.
  adapter.setPairingAllowed(true); adapter.poll(3);
  FakeBle::connect(31); adapter.poll(4);
  const int calls = FakeBle::advertiseCalls();
  FakeBle::disconnect(); adapter.poll(5);
  CHECK_MESSAGE(FakeBle::advertising(), "advertising again on USB");
  CHECK_MESSAGE(FakeBle::advertiseCalls() == calls + 1, "restarted with fresh data");
  adapter.setPairingAllowed(false); adapter.poll(6);
}
__attribute__((noinline)) void clobberStack() {
  volatile char junk[4096];
  for (size_t i = 0; i < sizeof junk; ++i) junk[i] = 'Z';
}
TEST_CASE("renamed unit advertises its new name after the phone leaves") {
  BleConfig adapter;
  adapter.setBonds(knownBonds());
  CHECK(adapter.begin(defaultConfig(), "0123456789abcdef", 0));
  adapter.applyAvailabilityPolicy(defaultConfig(), 0);
  adapter.poll(0);
  CHECK_EQ(std::string("Quiesco"), FakeBle::advertisedName());
  FakeBle::connect(40); adapter.poll(1);
  prove(); adapter.poll(2);
  REQUIRE(adapter.authenticated());
  const char name[] = "Bedroom";
  FakeBle::write(kName, reinterpret_cast<const uint8_t*>(name), sizeof name - 1);
  adapter.poll(3);
  Config changed;
  REQUIRE(adapter.takePendingConfig(changed));
  CHECK_EQ(std::string("Bedroom"), changed.deviceName);
  // The adapter must not hand ArduinoBLE a pointer into a dead stack frame.
  clobberStack();
  FakeBle::disconnect(); adapter.poll(4);
  CHECK(FakeBle::advertising());
  CHECK_EQ(std::string("Bedroom"), FakeBle::advertisedName());
  // A second rename keeps the first name readable until it is replaced.
  FakeBle::connect(41); adapter.poll(5);
  prove(); adapter.poll(6);
  const char other[] = "Nursery";
  FakeBle::write(kName, reinterpret_cast<const uint8_t*>(other), sizeof other - 1);
  adapter.poll(7);
  REQUIRE(adapter.takePendingConfig(changed));
  clobberStack();
  FakeBle::disconnect(); adapter.poll(8);
  CHECK_EQ(std::string("Nursery"), FakeBle::advertisedName());
}
TEST_CASE("proofs against no challenge do not use up attempts") {
  BleConfig adapter;
  adapter.setBonds(knownBonds());
  CHECK(adapter.begin(defaultConfig(), "0123456789abcdef", 0));
  FakeBle::connect(60);
  rngFails = true;  // the controller cannot draw a challenge
  adapter.poll(1);
  const auto state = FakeBle::read(kAuth);
  CHECK(std::all_of(state.begin() + 2, state.begin() + 18, [](uint8_t b) { return b == 0; }));
  uint8_t proof[20] = {2, 0, 0x34, 0x12};
  for (int i = 0; i < 8; ++i) {
    // A phone that read the zero challenge and proved against it.
    CHECK(BleCodec::authProof(key, state.data() + 2, 0x1234, proof + 4));
    FakeBle::write(kAuth, proof, sizeof proof);
    adapter.poll(2 + i);
    CHECK_FALSE(adapter.authenticated());
  }
  rngFails = false;
  adapter.poll(20);  // the next PROVE draws a challenge; this one has none yet
  FakeBle::write(kAuth, proof, sizeof proof);
  adapter.poll(21);
  prove(); adapter.poll(22);
  CHECK_MESSAGE(adapter.authenticated(),
                "a real challenge still has every attempt left");
  FakeBle::disconnect(); adapter.poll(23);
}
TEST_CASE("an unauthenticated write reads back as zeros, not as written") {
  BleConfig adapter;
  adapter.setBonds(knownBonds());
  CHECK(adapter.begin(defaultConfig(), "0123456789abcdef", 0));
  FakeBle::connect(70); adapter.poll(1);
  const uint8_t interval[4] = {0x08, 0x07, 0, 0};
  FakeBle::write(kInterval, interval, sizeof interval);
  const char name[] = "Forged";
  FakeBle::write(kName, reinterpret_cast<const uint8_t*>(name), sizeof name - 1);
  const std::vector<uint8_t> window = {1, 0, 0x78, 0, 0x82, 0x05, 0xA4, 0x01, 0xFF, 0xFF, 0xFF, 0xFF};
  FakeBle::write(kSleep, window.data(), static_cast<int>(window.size()));
  adapter.poll(2);
  for (const char* uuid : {kInterval, kName, kSleep}) {
    CAPTURE(uuid);
    const auto value = FakeBle::read(uuid);
    CHECK(std::all_of(value.begin(), value.end(), [](uint8_t b) { return b == 0; }));
  }
  Config changed;
  CHECK_FALSE(adapter.takePendingConfig(changed));
  prove(); adapter.poll(3);
  REQUIRE(adapter.authenticated());
  const auto shown = FakeBle::read(kInterval);
  CHECK_EQ(uint32_t{Config::kDefaultIntervalSeconds}, LittleEndian::getU32(shown.data()));
  FakeBle::disconnect(); adapter.poll(4);
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
TEST_CASE("status reports the log's newest record before the first measurement") {
  FlashSim::reset(); ticks = 0;
  W25Q64Flash flash; flash.begin(); ConfigStore store;
  CHECK_MESSAGE(store.save(flash, defaultConfig()), "seed config");
  CHECK_MESSAGE(store.saveBonds(flash, knownBonds()), "seed phone key");
  SampleLog original; Reading reading;
  for (int i = 0; i < 30; ++i) CHECK_MESSAGE(original.append(flash, reading), "seed log");
  // Reported 0 here, the app took the log for replaced and downloaded it all.
  App app; app.begin(); step(app, 1001); step(app);  // settings loaded, BLE up
  FakeBle::connect(22); step(app); prove(); step(app);
  CHECK_MESSAGE(newestSequence() == 30, "newest sequence known before the first append");
  FakeBle::disconnect();
}
TEST_CASE("a sync with no measurement due streams at once") {
  FlashSim::reset(); ticks = 0;
  W25Q64Flash flash; flash.begin(); ConfigStore store;
  CHECK_MESSAGE(store.save(flash, defaultConfig()), "seed config");
  CHECK_MESSAGE(store.saveBonds(flash, knownBonds()), "seed phone key");
  SampleLog original; Reading reading;
  for (int i = 0; i < 30; ++i) CHECK_MESSAGE(original.append(flash, reading), "seed log");
  App app; app.begin(); step(app, 1001); run(app, 40);  // first cycle done
  FakeBle::connect(23); step(app); prove(); step(app);
  const uint32_t newest = newestSequence();
  FakeBle::values().at(kLogData)->subscribed = true;
  FakeBle::values().at(kLogData)->bytes.clear();  // no END from an earlier test
  const uint8_t start[10] = {1, 0, 0, 0, 0, 0, 0, 0, 0xF4, 0};  // START 0, no limit, 244
  for (int round = 0; round < 2; ++round) {  // a retry or next batch: the same
    FakeBle::write(kSyncControl, start, sizeof start);
    int steps = 0;
    while (FakeBle::read(kLogData).empty() || FakeBle::read(kLogData)[0] != 2) {
      if (++steps > 60) break;
      step(app);
    }
    CHECK_MESSAGE(steps <= 60, "END within 600 ms: no measurement first");
    CHECK_MESSAGE(LittleEndian::getU32(FakeBle::read(kLogData).data() + 2) == newest + 1, "whole log sent");
    CHECK_MESSAGE(newestSequence() == newest, "no record appended");
    FakeBle::values().at(kLogData)->bytes.clear();
    run(app, 10);  // shut down to idle
  }
  FakeBle::disconnect();
}
bool adapterAuthenticated() { return (FakeBle::read(kAuth)[0] & 1) != 0; }
TEST_CASE("sync sessions: limits, fragments, abort, stall and disconnect") {
  FlashSim::reset(); ticks = 0;
  W25Q64Flash flash; flash.begin(); ConfigStore store;
  CHECK(store.save(flash, defaultConfig()));
  CHECK(store.saveBonds(flash, knownBonds()));
  SampleLog original; Reading reading;
  for (int i = 0; i < 30; ++i) REQUIRE(original.append(flash, reading));
  App app; app.begin(); step(app, 1001); run(app, 40);
  FakeBle::connect(28); step(app); prove(); step(app);
  const uint32_t newest = newestSequence();
  auto& log = FakeBle::values().at(kLogData);
  log->subscribed = true;
  auto syncing = [] { return (FakeBle::read(kStatus)[16] & 2) != 0; };
  auto start = [&](uint32_t first, uint16_t maxRecords, uint16_t payload) {
    uint8_t wire[10] = {1, 0};
    LittleEndian::putU32(wire + 2, first);
    LittleEndian::putU16(wire + 6, maxRecords);
    LittleEndian::putU16(wire + 8, payload);
    log->notified.clear();
    FakeBle::write(kSyncControl, wire, sizeof wire);
  };
  auto runUntilEnd = [&](int limit) {
    for (int i = 0; i < limit; ++i) {
      step(app);
      if (!log->notified.empty() && log->notified.back()[0] == 2) return true;
    }
    return false;
  };
  auto crcOk = [](const std::vector<uint8_t>& packet) {
    std::vector<uint8_t> copy = packet;
    copy[11] = 0;
    uint8_t crc = 0;
    for (uint8_t byte : copy) {
      crc ^= byte;
      for (int bit = 0; bit < 8; ++bit) crc = crc & 0x80 ? (crc << 1) ^ 0x07 : crc << 1;
    }
    return crc == packet[11];
  };

  // maxRecords: exactly that many, END names the next one.
  start(0, 2, 244);
  REQUIRE(runUntilEnd(200));
  uint32_t records = 0;
  for (const auto& packet : log->notified) {
    CHECK(crcOk(packet));
    if (packet[0] == 1) records += packet[6];
  }
  CHECK_EQ(uint32_t{2}, records);
  CHECK_EQ(uint32_t{3}, LittleEndian::getU32(log->notified.back().data() + 2));
  run(app, 10);

  // Past the newest: END only, naming the sequence asked for.
  start(newest + 5, 0, 244);
  REQUIRE(runUntilEnd(200));
  CHECK_EQ(size_t{1}, log->notified.size());
  CHECK_EQ(newest + 5, LittleEndian::getU32(log->notified.back().data() + 2));
  run(app, 10);

  // ATT payload 20: 7 fragments a record, counter +1 each, END carries the
  // next count; the fragments rebuild the records in order.
  start(5, 3, 20);
  REQUIRE(runUntilEnd(2000));
  uint8_t counter = 0;
  uint32_t fragments = 0;
  for (const auto& packet : log->notified) {
    CHECK(crcOk(packet));
    CHECK(packet.size() <= 20);
    CHECK_EQ(counter, packet[1]);
    if (packet[0] != 3) break;
    CHECK_EQ(5 + fragments / 7, LittleEndian::getU32(packet.data() + 2));
    CHECK_EQ(fragments % 7, packet[7]);
    ++fragments;
    ++counter;
  }
  CHECK_EQ(uint32_t{21}, fragments);
  CHECK_EQ(uint8_t{2}, log->notified.back()[0]);
  CHECK_EQ(uint32_t{8}, LittleEndian::getU32(log->notified.back().data() + 2));
  run(app, 10);

  // ABORT mid-stream: no END, the session ends, and a new START works.
  start(0, 0, 20);
  for (int i = 0; i < 100 && log->notified.size() < 10; ++i) step(app);
  REQUIRE(log->notified.size() >= 10);
  CHECK(syncing());
  const uint8_t abort[10] = {2, 0};
  FakeBle::write(kSyncControl, abort, sizeof abort);
  run(app, 20);
  CHECK_FALSE(syncing());
  for (const auto& packet : log->notified) CHECK(packet[0] != 2);
  start(newest + 1, 0, 244);
  CHECK(runUntilEnd(200));
  run(app, 10);

  // The phone stops taking notifications: the session ends 10 s later
  // without END.
  start(0, 0, 20);
  for (int i = 0; i < 100 && log->notified.size() < 5; ++i) step(app);
  log->subscribed = false;
  const size_t sent = log->notified.size();
  for (int i = 0; i < 1100 && syncing(); ++i) step(app);
  CHECK_FALSE(syncing());
  CHECK_EQ(sent, log->notified.size());
  log->subscribed = true;
  run(app, 10);

  // Disconnect mid-stream; the next connection's session starts afresh.
  start(0, 0, 20);
  for (int i = 0; i < 100 && log->notified.size() < 5; ++i) step(app);
  FakeBle::disconnect(); run(app, 20);
  CHECK_FALSE(syncing());
  FakeBle::connect(29); step(app);
  log->subscribed = true;
  // Not yet proved: START, factory reset and log erase are all ignored.
  start(0, 0, 244);
  const uint8_t reset[4] = {1, 0, 0xC7, 0xFA};
  FakeBle::write(kReset, reset, sizeof reset);
  run(app, 100);
  CHECK(log->notified.empty());
  prove(); step(app);
  REQUIRE(adapterAuthenticated());  // still enrolled: no reset happened
  CHECK_EQ(newest, newestSequence());
  log->subscribed = true;  // a new link subscribes again
  start(newest - 1, 0, 244);
  REQUIRE(runUntilEnd(200));
  CHECK_EQ(uint8_t{0}, log->notified.front()[1]);  // counter restarts
  CHECK_EQ(newest - 1, LittleEndian::getU32(log->notified.front().data() + 2));
  FakeBle::disconnect();
  log->bytes.clear();
  log->notified.clear();
}
TEST_CASE("an unlimited sync at the minimum MTU ends itself before the watchdog") {
  FlashSim::reset(); ticks = 0;
  W25Q64Flash flash; flash.begin(); ConfigStore store;
  CHECK(store.save(flash, defaultConfig()));
  CHECK(store.saveBonds(flash, knownBonds()));
  SampleLog original; Reading reading;
  for (int i = 0; i < 10000; ++i) REQUIRE(original.append(flash, reading));
  App app; app.begin(); step(app, 1001); run(app, 40);
  FakeBle::connect(25); step(app); prove(); step(app);
  const uint32_t newest = newestSequence();
  FakeBle::values().at(kLogData)->subscribed = true;
  FakeBle::values().at(kLogData)->bytes.clear();  // no END from an earlier test
  const uint8_t start[10] = {1, 0, 0, 0, 0, 0, 0, 0, 20, 0};  // START 0, no limit, 20
  FakeBle::write(kSyncControl, start, sizeof start);
  const uint32_t began = ticks;
  auto ended = [] {
    const auto packet = FakeBle::read(kLogData);
    return !packet.empty() && packet[0] == 2;
  };
  while (!ended() && ticks - began < 61UL * 60 * 1000) step(app, 30);
  REQUIRE(ended());
  CHECK(ticks - began <= 31UL * 60 * 1000);
  const uint32_t next = LittleEndian::getU32(FakeBle::read(kLogData).data() + 2);
  CHECK(next > 1);
  CHECK(next <= newest);  // stopped early: more to fetch
  // The client continues from END's cursor and gets the rest.
  FakeBle::values().at(kLogData)->bytes.clear();
  run(app, 10);
  uint8_t resume[10] = {1, 0, 0, 0, 0, 0, 0, 0, 0xF4, 0};
  LittleEndian::putU32(resume + 2, next);
  FakeBle::write(kSyncControl, resume, sizeof resume);
  const uint32_t resumed = ticks;
  while (!ended() && ticks - resumed < 10UL * 60 * 1000) step(app, 30);
  REQUIRE(ended());
  // Measurements went on meanwhile; the resume reaches the newest of them.
  CHECK(newestSequence() > newest);
  CHECK_EQ(newestSequence() + 1, LittleEndian::getU32(FakeBle::read(kLogData).data() + 2));
  FakeBle::disconnect();
}
TEST_CASE("app keeps measuring across the millis() rollover") {
  FlashSim::reset();
  ticks = UINT32_MAX - 400000;  // ~6.7 min before millis() wraps at 49.7 days
  W25Q64Flash flash; flash.begin(); ConfigStore store;
  CHECK(store.save(flash, defaultConfig()));
  CHECK(store.saveBonds(flash, knownBonds()));
  App app; app.begin(); step(app, 1001); run(app, 40);
  FakeBle::connect(26); step(app); prove(); step(app);
  const uint32_t first = newestSequence();
  REQUIRE(first >= 1);
  // Three intervals (300 s each), crossing the wrap in the second.
  const uint32_t intervalMs = Config::kDefaultIntervalSeconds * 1000;
  uint64_t elapsed = 0;
  bool wrapped = false;
  while (elapsed < 3ULL * intervalMs + 20000) {
    const uint32_t before = ticks;
    step(app, 100);
    elapsed += 100;
    wrapped |= ticks < before;
    CHECK_MESSAGE(static_cast<uint32_t>(ticks - lastKickTicks) < 30000,
                  "watchdog fed throughout");
    if (static_cast<uint32_t>(ticks - lastKickTicks) >= 30000) break;
  }
  CHECK(wrapped);
  CHECK_EQ(first + 3, newestSequence());
  FakeBle::disconnect();
  ticks = 0;
}
// Reads every record in the log, oldest first.
std::vector<SampleRecord> logRecords(W25Q64Flash& flash) {
  SampleLog log; std::vector<SampleRecord> out;
  if (!log.mount(flash)) return out;
  SampleLogCursor cursor; SampleRecord record;
  if (!log.startRead(flash, 0, cursor)) return out;
  while (log.readNext(flash, cursor, record)) out.push_back(record);
  return out;
}
TEST_CASE("records before, after and across clock changes") {
  constexpr char kEpoch[] = "7A1E0005-8E6F-4A7A-AE32-515549455343";
  FlashSim::reset(); ticks = 0;
  W25Q64Flash flash; flash.begin(); ConfigStore store;
  CHECK(store.save(flash, defaultConfig()));
  CHECK(store.saveBonds(flash, knownBonds()));
  App app; app.begin(); step(app, 1001); run(app, 40);
  const uint32_t intervalMs = Config::kDefaultIntervalSeconds * 1000;
  auto nextMeasurement = [&] {
    const uint32_t before = newestSequence();
    for (uint32_t t = 0; t < intervalMs + 30000 && newestSequence() == before; t += 100)
      step(app, 100);
    return newestSequence() == before + 1;
  };
  // Never synced: the record has no wall time, only ms since boot.
  auto records = logRecords(flash);
  REQUIRE(records.size() == 1);
  CHECK_EQ(uint32_t{0}, records[0].epochSeconds);
  CHECK(records[0].monotonicMs > 0);

  FakeBle::connect(27); step(app); prove(); step(app);
  constexpr uint64_t kSynced = 1791847042000ULL;
  uint8_t epoch[8]; LittleEndian::putU64(epoch, kSynced);
  FakeBle::write(kEpoch, epoch, sizeof epoch); step(app);
  REQUIRE(nextMeasurement());
  records = logRecords(flash);
  REQUIRE(records.size() == 2);
  CHECK(records[1].epochSeconds >= kSynced / 1000);
  CHECK(records[1].epochSeconds < kSynced / 1000 + 400);

  // The phone sets the clock two days back (a wrong phone clock): the log is
  // keyed by sequence, so appends go on and the time simply follows.
  LittleEndian::putU64(epoch, kSynced - 2ULL * 86400 * 1000);
  FakeBle::write(kEpoch, epoch, sizeof epoch); step(app);
  REQUIRE(nextMeasurement());
  // And forward by days.
  LittleEndian::putU64(epoch, kSynced + 5ULL * 86400 * 1000);
  FakeBle::write(kEpoch, epoch, sizeof epoch); step(app);
  REQUIRE(nextMeasurement());
  records = logRecords(flash);
  REQUIRE(records.size() == 4);
  CHECK(records[2].epochSeconds < records[1].epochSeconds);
  CHECK(records[3].epochSeconds > records[1].epochSeconds + 4 * 86400);
  for (size_t i = 1; i < records.size(); ++i)
    CHECK_EQ(records[i - 1].sequence + 1, records[i].sequence);
  FakeBle::disconnect();
}
TEST_CASE("the progress watchdog stops feeding only a stuck state") {
  FlashSim::reset(); ticks = 0;
  W25Q64Flash flash; flash.begin(); ConfigStore store;
  CHECK(store.save(flash, defaultConfig()));
  CHECK(store.saveBonds(flash, knownBonds()));
  App app; app.begin(); step(app, 1001); run(app, 40);
  // Idle is always fed, however long it lasts.
  for (int i = 0; i < 2000; ++i) {
    step(app, 100);
    REQUIRE(static_cast<uint32_t>(ticks - lastKickTicks) < 1000);
  }
  // A measurement whose sensor driver never finishes: fed for the state's
  // 60 s bound, then left to the hardware watchdog (30 s) to reset the unit.
  samplerHangs = true;
  const uint32_t before = newestSequence();
  uint32_t hungAt = 0;
  for (uint32_t t = 0; t < 400000; t += 100) {
    step(app, 100);
    if (static_cast<uint32_t>(ticks - lastKickTicks) >= 1000) { hungAt = ticks; break; }
  }
  REQUIRE(hungAt != 0);
  const uint32_t lastFed = lastKickTicks;
  for (int i = 0; i < 400; ++i) step(app, 100);  // 40 s: past the 30 s timeout
  CHECK_EQ(lastFed, lastKickTicks);  // never fed again
  CHECK_EQ(before, newestSequence());
  samplerHangs = false;
}
TEST_CASE("app ignores the reserved device control opcode 3") {
  FlashSim::reset(); ticks = 0; usbUpdateEntries = 0; onUsb = true;
  W25Q64Flash flash; flash.begin(); ConfigStore store;
  CHECK_MESSAGE(store.save(flash, defaultConfig()), "seed config");
  CHECK_MESSAGE(store.saveBonds(flash, knownBonds()), "seed phone key");
  App app; app.begin(); step(app, 1001); run(app, 40);  // first cycle done
  FakeBle::connect(22); step(app); prove(); step(app);
  const uint8_t reserved[4] = {3, 0, 0xC7, 0xFA};
  FakeBle::write(kReset, reserved, sizeof reserved); run(app, 100);
  CHECK_MESSAGE(bool(FakeBle::peer()), "link kept");
  CHECK_MESSAGE(usbUpdateEntries == 0, "no bootloader entered");
  FakeBle::disconnect();
  onUsb = false;
}
TEST_CASE("app enters the USB update drive") {
  FlashSim::reset(); ticks = 0; usbUpdateEntries = 0; onUsb = false;
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
  CHECK_MESSAGE(usbUpdateEntries == 1, "UF2 bootloader entered");
  onUsb = false;
}
TEST_CASE("app with a dead flash backs off and still updates") {
  FlashSim::reset(); ticks = 0; usbUpdateEntries = 0; onUsb = true;
  W25Q64Flash flash; flash.begin(); ConfigStore store;
  CHECK(store.save(flash, defaultConfig()));
  CHECK(store.saveBonds(flash, knownBonds()));
  App app; app.begin(); step(app, 1001); run(app, 40);
  FakeBle::connect(24); step(app); prove(); step(app);
  FlashSim::setChipAnswers(false);
  // A measurement (the erase command rides one), and while its sensors warm
  // up a setting that can never be saved: the persist step finds the flash
  // dead with the setting's retry time already due.
  const uint8_t erase[8] = {2, 0, 0xC7, 0xFA, 0xFF, 0xFF, 0xFF, 0xFF};
  FakeBle::write(kReset, erase, sizeof erase);
  step(app, 100); step(app, 100);
  const uint32_t before = FlashSim::begins();
  const std::vector<uint8_t> window = {1, 0, 0x78, 0, 0x82, 0x05, 0xA4, 0x01, 0xFF, 0xFF, 0xFF, 0xFF};
  FakeBle::write(kSleep, window.data(), static_cast<int>(window.size()));
  const uint32_t start = ticks;
  while (ticks - start < 50000) step(app, 100);
  // Only the persist step; before the fix a config cycle followed at once.
  CHECK_EQ(before + 1, FlashSim::begins());

  // A USB update request cannot wait for settings that will never save.
  const uint8_t usbUpdate[4] = {4, 0, 0xC7, 0xFA};
  FakeBle::write(kReset, usbUpdate, sizeof usbUpdate);
  const uint32_t requested = ticks;
  while (usbUpdateEntries == 0 && ticks - requested < 6 * 60 * 1000) step(app, 100);
  CHECK_EQ(1, usbUpdateEntries);
  CHECK(ticks - requested >= 5 * 60 * 1000);
  FlashSim::setChipAnswers(true);
  FakeBle::disconnect();
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
    std::vector<uint8_t> bad = golden; bad[10] = 0x00; bad[11] = 0x01;  // reserved bytes 8-11 not FF
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
TEST_CASE("a watchdog reset keeps the clock and is recorded") {
  constexpr char kEpoch[] = "7A1E0005-8E6F-4A7A-AE32-515549455343";
  constexpr char kLastReset[] = "7A1E0013-8E6F-4A7A-AE32-515549455343";
  FlashSim::reset(); ticks = 0; nextResetBits = 0; retainedValid = false;
  W25Q64Flash flash; flash.begin(); ConfigStore store;
  CHECK_MESSAGE(store.save(flash, defaultConfig()), "seed config");
  CHECK_MESSAGE(store.saveBonds(flash, knownBonds()), "seed phone key");
  constexpr uint64_t kSyncedEpochMs = 1791847042000ULL;
  uint32_t firstBoot = 0;
  {
    App app; app.begin(); step(app, 1001); run(app, 40);
    FakeBle::connect(30); step(app); prove(); step(app);
    uint8_t epoch[8]; LittleEndian::putU64(epoch, kSyncedEpochMs);
    FakeBle::write(kEpoch, epoch, sizeof epoch); step(app);
    FakeBle::disconnect();
    run(app, 100);
    CHECK_MESSAGE(retainedValid, "breadcrumb stored at every feed");
    firstBoot = retainedBreadcrumb.bootCount;
    CHECK_MESSAGE(firstBoot != 0, "breadcrumb carries the boot counter");
    CHECK_MESSAGE(retainedBreadcrumb.heapKiB == 42, "heap sampled at the cycle start");
  }
  const uint64_t epochAtFeed = retainedBreadcrumb.feedEpochMs;
  CHECK_MESSAGE(epochAtFeed > kSyncedEpochMs, "feed time follows the synced clock");
  const uint64_t uptimeAtFeed = retainedBreadcrumb.feedUptimeMs;

  // The watchdog fires; the next boot starts its own clock from zero.
  nextResetBits = ResetReason::kWatchdog; ticks = 0;
  {
    App app; app.begin(); step(app, 1001); run(app, 40);
    FakeBle::connect(31); step(app); prove(); step(app);
    const auto epoch = FakeBle::read(kEpoch);
    CHECK_MESSAGE((epoch.size() == 8 && LittleEndian::getU64(epoch.data()) ==
                   epochAtFeed + HardwareWatchdog::kTimeoutMs),
                  "clock restored at one watchdog timeout after the last feed");
    const auto wire = FakeBle::read(kLastReset);
    ResetHistory::Record record;
    CHECK_MESSAGE(ResetHistory::decode(wire.data(), wire.size(), record), "0013 decodes");
    CHECK_MESSAGE(record.bootCount == firstBoot, "names the boot that ended");
    CHECK_MESSAGE(record.uptimeS == uptimeAtFeed / 1000, "with its uptime");
    CHECK_MESSAGE(record.cause == ResetHistory::kCauseWatchdog, "and the cause");
    CHECK_MESSAGE(record.heapKiB == 42, "and the heap then");
    FakeBle::disconnect();
  }

  // A power cut: no RAM, no clock, but the record stays in flash.
  nextResetBits = 0; retainedValid = false; ticks = 0;
  {
    App app; app.begin(); step(app, 1001); run(app, 40);
    FakeBle::connect(32); step(app); prove(); step(app);
    const auto epoch = FakeBle::read(kEpoch);
    CHECK_MESSAGE((epoch.size() == 8 && LittleEndian::getU64(epoch.data()) == 0),
                  "power-on reset loses the clock");
    const auto wire = FakeBle::read(kLastReset);
    ResetHistory::Record record;
    CHECK_MESSAGE((ResetHistory::decode(wire.data(), wire.size(), record) &&
                   record.bootCount == firstBoot &&
                   record.cause == ResetHistory::kCauseWatchdog),
                  "last unexpected reset survives a power cut");
    FakeBle::disconnect();
  }
  retainedValid = false;
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
  if (rngFails) return 1;  // HCI error status
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
bool EnvironmentalSampler::ready() const { return !samplerHangs; }
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
void HardwareWatchdog::kick() { lastKickTicks = ticks; }
void BootloaderEntry::enterUsbUpdate() { ++usbUpdateEntries; }
namespace DeviceId { uint64_t read() { return 123; } }
namespace ResetReason { uint32_t take() { return nextResetBits; } const char* describe(uint32_t) { return "test"; } }
namespace RetainedRam {
void take(ResetHistory::Breadcrumb& breadcrumb, bool& hasBreadcrumb,
          ResetHistory::FaultCapture&, bool& hasFault) {
  hasBreadcrumb = retainedValid;
  if (retainedValid) breadcrumb = retainedBreadcrumb;
  retainedValid = false;
  hasFault = false;
}
void store(const ResetHistory::Breadcrumb& breadcrumb) { retainedBreadcrumb = breadcrumb; retainedValid = true; }
uint16_t heapKiB() { return 42; }
}
namespace TraceStore { bool flush(W25Q64Flash&) { return true; } void dump(W25Q64Flash&) {} }
