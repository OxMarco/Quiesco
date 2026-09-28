// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <Arduino.h>
#include <ArduinoBLE.h>
#include <Wire.h>
#include <utility/HCI.h>
#include "App.h"
#include "diagnostics/DebugLog.h"
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
namespace {
uint32_t ticks = 0;
int failures = 0;
std::function<void()> rngEvent;
constexpr char kAuth[] = "7A1E0010-8E6F-4A7A-AE32-515549455343";
constexpr char kInterval[] = "7A1E0001-8E6F-4A7A-AE32-515549455343";
constexpr char kReset[] = "7A1E000D-8E6F-4A7A-AE32-515549455343";
constexpr char kReading[] = "7A1E0003-8E6F-4A7A-AE32-515549455343";
const uint8_t key[16] = {1, 2, 3};
void expect(bool condition, const char* message) {
  if (!condition) { std::cerr << message << '\n'; ++failures; }
}
BondTable knownBonds() {
  BondTable bonds;
  const uint8_t address[6] = {0x34, 0x12};
  bonds.storeLtk(address, key);
  return bonds;
}
void prove() {
  const auto state = FakeBle::read(kAuth);
  uint8_t proof[20] = {2, 0, 0x34, 0x12};
  expect(BleCodec::authProof(key, state.data() + 2, 0x1234, proof + 4), "proof creation");
  FakeBle::write(kAuth, proof, sizeof proof);
}
void step(App& app, uint32_t elapsed = 10) { ticks += elapsed; app.step(); }
void run(App& app, int steps) { for (int i = 0; i < steps; ++i) step(app); }
void testReconnect() {
  BleConfig adapter;
  adapter.setBonds(knownBonds());
  expect(adapter.begin(defaultConfig(), "0123456789abcdef", ticks), "BLE begin");
  FakeBle::connect(1); adapter.poll(ticks);
  prove(); adapter.poll(ticks);
  expect(adapter.authenticated(), "known phone authenticates");
  Reading reading; reading.temperatureC = 23; reading.valid = VALID_TEMPERATURE;
  adapter.publishReading(reading);
  // One HCI poll drains disconnect, reconnect, a read and a write.
  FakeBle::events().push_back([&]() {
    FakeBle::disconnect(); FakeBle::connect(2);
    const auto bytes = FakeBle::read(kReading);
    expect(std::all_of(bytes.begin(), bytes.end(), [](uint8_t b) { return b == 0; }),
           "new peer must see zeros before poll returns");
    const uint8_t interval[4] = {60, 0, 0, 0};
    FakeBle::write(kInterval, interval, sizeof interval);
  });
  adapter.poll(ticks);
  Config changed;
  expect(!adapter.authenticated(), "replacement peer must not inherit authorization");
  expect(!adapter.takePendingConfig(changed), "replacement peer write must be rejected");
  // A write authorized on the old link cannot be replayed by the new one.
  prove(); adapter.poll(ticks);
  const uint8_t interval[4] = {60, 0, 0, 0};
  FakeBle::write(kInterval, interval, sizeof interval);
  FakeBle::disconnect(); FakeBle::connect(3); adapter.poll(ticks);
  expect(!adapter.takePendingConfig(changed), "discard previous session's pending writes");
  // More than 256 failed proofs must not wrap the five-attempt limit.
  uint8_t wrong[20] = {2, 0, 0x34, 0x12};
  for (int i = 0; i < 260; ++i) {
    FakeBle::write(kAuth, wrong, sizeof wrong); adapter.poll(ticks);
  }
  prove(); adapter.poll(ticks);
  expect(!adapter.authenticated(), "failed proof limit must saturate");
  FakeBle::disconnect();
  FakeBle::connect(6); adapter.poll(ticks);
  prove();
  rngEvent = [] { FakeBle::disconnect(); FakeBle::connect(7); };
  adapter.poll(ticks);
  expect(!adapter.authenticated(), "connection change during HCI RNG must invalidate accepted proof");
  FakeBle::disconnect();
}
void testEnrollment() {
  constexpr char metadata[] = "7A1E0011-8E6F-4A7A-AE32-515549455343";
  const uint8_t enrol[2] = {1, 0};
  BleConfig adapter;
  BondTable empty; adapter.setBonds(empty);
  adapter.begin(defaultConfig(), "0123456789abcdef", 0);
  adapter.setPairingAllowed(false);
  FakeBle::connect(10); adapter.poll(0);
  FakeBle::write(kAuth, enrol, sizeof enrol); adapter.poll(1);
  expect(!adapter.enrollmentPending(), "enrollment requires USB");
  adapter.setPairingAllowed(true);
  FakeBle::write(kAuth, enrol, sizeof enrol); adapter.poll(2);
  expect(adapter.enrollmentPending(), "USB allows a pending on-screen key");
  expect(adapter.bondCount() == 0, "unauthorized ENROL must not save or evict keys");
  uint32_t code = 0;
  expect(adapter.takeEnrollment(code) && code < BleCodec::kSetupCodeCount,
         "local display gets a six-digit code");
  auto wire = FakeBle::read(metadata);
  uint16_t keyId = LittleEndian::getU16(wire.data());
  expect(keyId != 0, "radio exposes public key id");
  expect(std::all_of(wire.begin() + 2, wire.end(), [](uint8_t b) { return b == 0; }),
         "no key appears in enrollment characteristic before the code is proved");
  // Repeated ENROL does not replace the code being typed or extend its lifetime.
  FakeBle::write(kAuth, enrol, sizeof enrol); adapter.poll(1000);
  expect(!adapter.takeEnrollment(code), "ENROL retries keep the current screen");
  uint8_t setup[16];
  expect(BleCodec::setupKey(code, keyId, setup), "derive setup key");
  auto state = FakeBle::read(kAuth);
  uint8_t proof[20] = {2, 0}; LittleEndian::putU16(proof + 2, keyId);
  // A wrong code is refused and leaves setup pending.
  uint8_t wrongSetup[16];
  BleCodec::setupKey((code + 1) % BleCodec::kSetupCodeCount, keyId, wrongSetup);
  BleCodec::authProof(wrongSetup, state.data() + 2, keyId, proof + 4);
  FakeBle::write(kAuth, proof, sizeof proof); adapter.poll(1001);
  expect(!adapter.authenticated() && adapter.bondCount() == 0, "wrong code must not enroll");
  state = FakeBle::read(kAuth);
  BleCodec::authProof(setup, state.data() + 2, keyId, proof + 4);
  FakeBle::write(kAuth, proof, sizeof proof); adapter.poll(1002);
  expect(adapter.authenticated() && adapter.bondCount() == 1, "only proof of display code enrolls");
  expect(!adapter.enrollmentPending(), "successful proof scrubs pending setup");
  // The phone is handed a fresh full-length key, not one derived from the code.
  wire = FakeBle::read(metadata);
  expect(LittleEndian::getU16(wire.data()) == keyId, "issued key carries its id");
  uint8_t issued[16];
  std::copy(wire.begin() + 4, wire.end(), issued);
  expect(std::any_of(issued, issued + 16, [](uint8_t b) { return b != 0; }) &&
             !std::equal(issued, issued + 16, setup),
         "issued key is random, not the setup key");
  adapter.poll(1002 + 30000);
  wire = FakeBle::read(metadata);
  expect(std::all_of(wire.begin(), wire.end(), [](uint8_t b) { return b == 0; }),
         "issued key is withdrawn after 30 s");
  FakeBle::disconnect(); FakeBle::connect(12); adapter.poll(40000);
  state = FakeBle::read(kAuth);
  BleCodec::authProof(setup, state.data() + 2, keyId, proof + 4);
  FakeBle::write(kAuth, proof, sizeof proof); adapter.poll(40001);
  expect(!adapter.authenticated(), "the setup key is not a phone key");
  state = FakeBle::read(kAuth);
  BleCodec::authProof(issued, state.data() + 2, keyId, proof + 4);
  FakeBle::write(kAuth, proof, sizeof proof); adapter.poll(40002);
  expect(adapter.authenticated(), "the issued key signs in");
  FakeBle::disconnect(); FakeBle::connect(11); adapter.poll(42000);
  FakeBle::write(kAuth, enrol, sizeof enrol); adapter.poll(42001);
  expect(adapter.takeEnrollment(code), "second enrollment can start");
  keyId = LittleEndian::getU16(FakeBle::read(metadata).data());
  BleCodec::setupKey(code, keyId, setup);
  adapter.poll(222002);
  state = FakeBle::read(kAuth);
  LittleEndian::putU16(proof + 2, keyId);
  BleCodec::authProof(setup, state.data() + 2, keyId, proof + 4);
  FakeBle::write(kAuth, proof, sizeof proof); adapter.poll(222003);
  expect(!adapter.authenticated() && adapter.bondCount() == 1, "expired setup code cannot enroll");
  FakeBle::write(kAuth, enrol, sizeof enrol); adapter.poll(222004);
  adapter.setPairingAllowed(false);
  expect(!adapter.enrollmentPending(), "unplugging cancels setup immediately");
  FakeBle::disconnect();
}
void testFrcRetry() {
  Scd41Sensor sensor;
  expect(sensor.begin(), "SCD begin");
  sensor.startFrcRunUp(0); sensor.pollFrcRunUp(0);
  sensorReady = false;
  expect(sensor.pollFrcRunUp(5000) == Scd41Sensor::FrcProgress::kRunning,
         "not-ready at five seconds must be retried");
  sensorReady = true;
  expect(sensor.pollFrcRunUp(5100) == Scd41Sensor::FrcProgress::kRunning,
         "slightly late valid measurement must continue run-up");
  for (uint64_t shot = 1; shot < 5; ++shot) {
    sensor.pollFrcRunUp(shot * 60000);
    sensor.pollFrcRunUp(shot * 60000 + 5000);
  }
  expect(sensor.pollFrcRunUp(300000) == Scd41Sensor::FrcProgress::kReady,
         "all five shots complete preparation");
  int16_t correction = 0;
  expect(sensor.performForcedRecalibration(450, correction), "prepared FRC succeeds");
  expect(!sensor.performForcedRecalibration(450, correction), "preparation used once");
  sensor.startFrcRunUp(0); sensor.pollFrcRunUp(0); sensorReady = false;
  expect(sensor.pollFrcRunUp(7000) == Scd41Sensor::FrcProgress::kFailed,
         "stuck not-ready must fail at deadline");
  sensorReady = true;
}
void testAppEraseRecovery() {
  FlashSim::reset(); ticks = 0;
  W25Q64Flash flash; flash.begin();
  ConfigStore config;
  expect(config.save(flash, defaultConfig()), "save config");
  expect(config.saveBonds(flash, knownBonds()), "save phone key");
  SampleLog original;
  Reading reading;
  for (int i = 0; i < 200; ++i) expect(original.append(flash, reading), "seed log");
  {
    App app; app.begin(); step(app, 1001); run(app, 20);
    FakeBle::connect(4); step(app); prove(); step(app);
    const uint8_t reset[4] = {1, 1, 0xC7, 0xFA};
    FakeBle::write(kReset, reset, sizeof reset);
    const auto before = FlashSim::sampleSectorErases();
    for (int i = 0; i < 250 && FlashSim::sampleSectorErases() == before; ++i) step(app);
    expect(FlashSim::sampleSectorErases() > before, "factory reset starts erase");
    ConfigStore journal; uint32_t floor = 0;
    expect(journal.loadLogErase(flash, floor) && floor >= 202, "intent durable before first erase");
    // Destroy the application while almost all old sectors remain.
    FakeBle::disconnect();
  }
  {
    App reboot; reboot.begin(); step(reboot, 1001);
    const uint32_t sectors = FlashStorageLayout::kSampleDbBytes / FlashStorageLayout::kSectorBytes;
    run(reboot, sectors + 40);
    ConfigStore journal; uint32_t floor = 99;
    expect(journal.loadLogErase(flash, floor) && floor == 0, "reboot finishes deletion and clears journal");
    SampleLog recovered; SampleLogCursor cursor; SampleRecord record;
    expect(recovered.startRead(flash, 0, cursor), "new log readable");
    expect(recovered.readNext(flash, cursor, record) && record.sequence >= 202,
           "new sequence cannot reuse pre-reset history");
    expect(!recovered.readNext(flash, cursor, record), "no old records survive resumed erase");
  }
}
void testAppKeepsEraseIntentUntilAppend() {
  FlashSim::reset(); ticks = 0;
  W25Q64Flash flash; flash.begin(); ConfigStore store;
  expect(store.save(flash, defaultConfig()), "seed config for final-erase interruption");
  expect(store.saveLogErase(flash, 42), "seed pending erase floor");
  const uint32_t sectors = FlashStorageLayout::kSampleDbBytes / FlashStorageLayout::kSectorBytes;
  {
    App app; app.begin(); step(app, 1001);
    for (uint32_t i = 0; i < sectors + 30 && FlashSim::sampleSectorErases() < sectors - 1; ++i) step(app);
    expect(FlashSim::sampleSectorErases() == sectors - 1, "stop before final sector");
    FlashSim::failNextPrograms(1);  // first fresh append/format cannot commit
    step(app);
    ConfigStore check; uint32_t floor = 0;
    expect(check.loadLogErase(flash, floor) && floor == 42,
           "failed first append must retain durable erase intent");
  }
  App reboot; reboot.begin(); step(reboot, 1001); run(reboot, sectors + 40);
  ConfigStore check; uint32_t floor = 0;
  expect(check.loadLogErase(flash, floor) && floor == 0, "healthy reboot finishes interrupted reset");
  SampleLog log; SampleLogCursor cursor; SampleRecord record;
  expect(log.startRead(flash, 0, cursor) && log.readNext(flash, cursor, record) && record.sequence == 42,
         "sequence floor survives a completely erased partition and reboot");
}
void testAppRetriesConfigRead() {
  FlashSim::reset(); ticks = 0;
  W25Q64Flash flash; flash.begin(); ConfigStore store;
  Config saved = defaultConfig(); saved.measurementIntervalSeconds = 60;
  expect(store.save(flash, saved), "seed settings");
  expect(store.saveBonds(flash, knownBonds()), "seed bonds");
  FlashSim::failReadAt(1);
  App app; app.begin(); step(app, 1001); run(app, 20);
  ConfigStore intact; Config loaded;
  expect(intact.load(flash, loaded) && loaded.measurementIntervalSeconds == 60,
         "failed boot read must not replace config with defaults");
  step(app, 300001); step(app, 1001); run(app, 20);
  FakeBle::connect(5); step(app); prove(); step(app);
  const auto interval = FakeBle::read(kInterval);
  expect(interval.size() == 4 && LittleEndian::getU32(interval.data()) == 60,
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
bool BatteryMonitor::sample(float& v) { v = 3.8f; return true; }
bool BatteryMonitor::pollCharging(uint64_t) { return false; }
bool BatteryMonitor::usbPowered() const { return false; }
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
namespace DeviceId { uint64_t read() { return 123; } }
namespace ResetReason { uint32_t take() { return 0; } const char* describe(uint32_t) { return "test"; } }
namespace TraceStore { bool flush(W25Q64Flash&) { return true; } void dump(W25Q64Flash&) {} }
int main() {
  testReconnect(); testEnrollment(); testFrcRetry(); testAppEraseRecovery(); testAppKeepsEraseIntentUntilAppend(); testAppRetriesConfigRead();
  if (failures) return EXIT_FAILURE;
  std::cout << "All Quiesco runtime state-machine tests passed\n";
  return EXIT_SUCCESS;
}
