// SPDX-License-Identifier: GPL-3.0-only
// Quiesco unit test, and the factory test (HARDWARE.md §9).
//
// Checks the whole board: BME280, VEML7700, SCD41, PDM mic, battery, the
// W25Q64 flash, the e-ink panel and the BLE radio. Press 't' for a PASS/FAIL
// verdict per part; the other commands (press '?') narrow a failure down to
// the rail, the bus, or one part. HARDWARE.md §7-8 explains how to read the
// results.
//
// This compiles the *production* drivers -- `src/` here is a tree of symlinks
// into the firmware's own `src/`, not a copy. A result found here therefore
// transfers to the firmware unchanged; a sketch with its own bring-up sequence
// would only ever prove that a second sequence works.
//
// Board: XIAO nRF52840 (Sense) Plus, Seeeduino mbed 2.9.3. Serial at 115200.
//
// Blocking delays are used freely below: unlike the firmware there is no BLE
// stack to service, and a spike that reads like a state machine is harder to
// trust than one that reads top to bottom.

#include <Arduino.h>
#include <ArduinoBLE.h>
#include <SensirionI2cScd4x.h>
#include <Wire.h>

#if !defined(ARDUINO_SEEED_XIAO_NRF52840_PLUS) && \
    !defined(ARDUINO_SEEED_XIAO_NRF52840_SENSE_PLUS)
#error "Select board: XIAO nRF52840 (Sense) Plus - Seeed nRF52 mbed core"
#endif

#include "src/board/PowerDomain.h"
#include "src/drivers/BatteryMonitor.h"
#include "src/drivers/Bme280Sensor.h"
#include "src/drivers/EpaperDisplay.h"
#include "src/drivers/PdmMicrophone.h"
#include "src/drivers/Scd41Sensor.h"
#include "src/drivers/Veml7700Sensor.h"
#include "src/drivers/W25Q64Flash.h"
#include "src/model/Reading.h"
#include "src/platform/DeviceId.h"
#include "src/services/UnitIdentity.h"
#include "src/storage/FlashStorageLayout.h"

namespace {

constexpr uint32_t kBusClockHz = 100000;  // margin: every part on the bus allows 400 kHz
constexpr uint32_t kRailOffMs = 2000;     // long enough for the rail to bleed
constexpr uint32_t kSweepWindowMs = 3000;
constexpr uint32_t kSweepStepMs = 10;
constexpr uint8_t kSweepRounds = 8;
constexpr uint32_t kCycleGapMs = 5000;
constexpr uint32_t kSettleStepMs = 250;
constexpr uint32_t kSettleMaxMs = 5000;
constexpr uint32_t kMonitorStepMs = 100;
constexpr uint32_t kMonitorHeartbeatMs = 2000;
constexpr uint32_t kBleAdvertiseMs = 60000;  // long enough to find it on a phone

// The flash check erases and programs only this sector, which no partition
// owns, so it is safe on a unit that already holds a config and a log.
constexpr uint32_t kTestSector = FlashStorageLayout::kTestSectorOffset;
static_assert(kTestSector + W25Q64Flash::kSectorBytes ==
                  W25Q64Flash::kCapacityBytes,
              "the flash test sector must be the chip's last sector");
static_assert(FlashStorageLayout::kSectorBytes == W25Q64Flash::kSectorBytes,
              "layout and driver disagree on the sector size");

struct ProbeTarget {
  uint8_t address;
  const char* name;
};

// Both BME280 addresses are probed because the driver tries each in turn; only
// one of them can ever answer.
const ProbeTarget kProbes[] = {
    {0x76, "bme280@76"},
    {0x77, "bme280@77"},
    {0x10, "veml7700@10"},
    {0x62, "scd41@62"},
};
constexpr uint8_t kProbeCount = sizeof(kProbes) / sizeof(kProbes[0]);

PowerDomain power;
Bme280Sensor bme280;
Veml7700Sensor veml7700;
Scd41Sensor scd41;
PdmMicrophone microphone;
BatteryMonitor batteryMonitor;
W25Q64Flash flash;
EpaperDisplay display;

char unitSerial[UnitIdentity::kSerialChars + 1];
char bleName[32];  // "Quiesco TEST XXXX", the suffix matching the unit's name
bool bleStarted = false;
bool bleAdvertising = false;
bool bleWasConnected = false;
uint32_t bleAdvertiseUntilMs = 0;

// Tunable at runtime, unlike PowerDomain::kSettleMs, so the rail-settle
// hypothesis can be tested without a reflash per data point. Seeded from the
// production value so an untouched run reproduces firmware behaviour.
uint32_t settleMs = PowerDomain::kSettleMs;

bool enableBme280 = true;
bool enableVeml7700 = true;
bool enableScd41 = true;
bool enableMicrophone = true;
bool enableBattery = true;
bool cycling = false;
bool unitTest = false;  // 't': one cycle, then a PASS/FAIL verdict
// Set by the sensor verdicts; the board checks run once the rail is down.
bool boardChecksPending = false;
bool sensorsPassed = false;

enum class Phase : uint8_t {
  kIdle,
  kSettling,
  kInitializing,
  kAcquiring,
  kWaiting,
  kReporting,
  kCooldown,
};

Phase phase = Phase::kIdle;
Reading reading;
unsigned long cycleNumber = 0;
uint32_t railOnAtMs = 0;
uint32_t acquireAtMs = 0;
uint32_t cooldownUntilMs = 0;

// Per-sensor latency from start() to ready(), the number that says whether the
// SCD41's 7 s timeout budget is generous or already marginal. -1 = never got
// there, -2 = the sensor was absent so the question does not apply.
int32_t bmeDoneMs = -1;
int32_t vemlDoneMs = -1;
int32_t scdDoneMs = -1;
int32_t micDoneMs = -1;

uint64_t nowMs() {
  // A spike runs for minutes, so the 49-day millis() rollover the firmware's
  // MonotonicClock exists to handle is not worth pulling in here.
  return static_cast<uint64_t>(millis());
}

// An empty Wire transaction is sent as a 1-byte *read* on this mbed core, and
// the SCD41 NAKs its read header while it has nothing queued -- every SCD41
// "never answered" this sketch printed before that was known came from here.
// It gets a real idle-mode command instead (get_serial_number, reply left
// unread); the other parts ACK a read at any time.
bool acknowledges(uint8_t address) {
  Wire.beginTransmission(address);
  if (address == SCD41_I2C_ADDR_62) {
    Wire.write(0x36);
    Wire.write(0x82);
  }
  return Wire.endTransmission() == 0;
}

// The mbed core's Wire.end() deletes its I2C object without nulling the
// pointer, so a second end() is a double free that hangs the board (seen when
// 'w' followed 's'). Every begin/end goes through this flag, as
// EnvironmentalSampler does with busStarted_.
bool busActive = false;

void busDown() {
  if (busActive) {
    Wire.end();
    busActive = false;
  }
}

void busUp(uint32_t clockHz = kBusClockHz) {
  busDown();
  Wire.begin();
  Wire.setClock(clockHz);
  busActive = true;
}

// R14/R15 hang off the *switched* +3.3V, so with the rail down the bus has no
// pull-ups at all and every probe NAKs regardless of what is out there. The
// back-power test would then always report "silent" and prove nothing. These
// are the nRF's internal pull-ups, enabled only for that test.
//
// Raw port pin numbers are unavoidable here: the TWI pins belong to Wire, and
// BoardPins deliberately does not export them (see its closing comment).
// P0.04 = SDA, P0.05 = SCL, per HARDWARE.md §3. S0D1 keeps them open-drain.
constexpr uint32_t kSdaPortPin = 4;
constexpr uint32_t kSclPortPin = 5;

void enableInternalPullup(uint32_t portPin) {
  NRF_P0->PIN_CNF[portPin] =
      (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos) |
      (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos) |
      (GPIO_PIN_CNF_PULL_Pullup << GPIO_PIN_CNF_PULL_Pos) |
      (GPIO_PIN_CNF_DRIVE_S0D1 << GPIO_PIN_CNF_DRIVE_Pos) |
      (GPIO_PIN_CNF_SENSE_Disabled << GPIO_PIN_CNF_SENSE_Pos);
}

void railUpAndSettle() {
  power.enable(nowMs());
  delay(settleMs);
  busUp();
}

void railDown() {
  busDown();
  power.disable();
}

void printValue(const char* label, float value, bool valid, int decimals) {
  Serial.print(label);
  if (valid) {
    Serial.print(value, decimals);
  } else {
    Serial.print("n/a");
  }
}

void printState(const char* label, bool present, bool timedOut, bool got) {
  Serial.print(label);
  if (!present) {
    Serial.print("missing");
  } else if (timedOut) {
    Serial.print("timeout");
  } else if (got) {
    Serial.print("ok");
  } else {
    Serial.print("read-error");
  }
}

void printLatency(const char* label, int32_t value) {
  Serial.print(label);
  if (value == -2) {
    Serial.print("n/a");
  } else if (value < 0) {
    Serial.print("never");
  } else {
    Serial.print(value);
    Serial.print("ms");
  }
}

void printHelp() {
  Serial.println();
  Serial.println("commands:");
  Serial.println("  ?  this help");
  Serial.println("  t  unit test -- sensors, flash, display, BLE; PASS/FAIL per part (start here)");
  Serial.println("  s  settle sweep -- when does each address first ACK after rail-on");
  Serial.println("  i  I2C scan (rail left on)");
  Serial.println("  c  toggle continuous measurement cycles");
  Serial.println("  d  dump SCD41 persistent settings (ASC, offsets)");
  Serial.println("  b  back-power probe -- does anything answer with the rail off");
  Serial.println("  w  SCD41 wake probe -- rule out power-down state");
  Serial.println("  m  live monitor -- press/flex U6 while it runs, catches a cracked joint");
  Serial.println("  f  bus speed sweep -- a resistive joint may pass at 10 kHz");
  Serial.println("  x  exhaustive SCD41 probe -- bit-bang, line check, resets, driver");
  Serial.println("  n  flash check -- JEDEC ID, erase, program, verify the test sector");
  Serial.println("  e  display check -- full refresh with a test pattern");
  Serial.println("  a  BLE check -- advertise for 60 s, find it on a phone");
  Serial.println("  1  BME280   2  VEML7700   3  SCD41   4  microphone   5  battery");
  Serial.println("  +  settle +250ms   -  settle -250ms");
  Serial.print("settle=");
  Serial.print(settleMs);
  Serial.print("ms (firmware uses ");
  Serial.print(PowerDomain::kSettleMs);
  Serial.print("ms)  bme=");
  Serial.print(enableBme280 ? "on" : "off");
  Serial.print(" veml=");
  Serial.print(enableVeml7700 ? "on" : "off");
  Serial.print(" scd=");
  Serial.print(enableScd41 ? "on" : "off");
  Serial.print(" mic=");
  Serial.print(enableMicrophone ? "on" : "off");
  Serial.print(" batt=");
  Serial.println(enableBattery ? "on" : "off");
}

// The decisive measurement: PowerDomain::kSettleMs is 1000 ms and the SCD41
// needs ~1000 ms after power-up before it ACKs at all. If it answers late even
// once, Scd41Sensor::begin() silently marks the part unavailable for the whole
// cycle and CO2 goes missing with nothing else looking wrong.
void runSettleSweep() {
  Serial.println();
  Serial.print("settle sweep: ");
  Serial.print(kSweepRounds);
  Serial.print(" rounds, rail off ");
  Serial.print(kRailOffMs);
  Serial.print("ms, then probe every ");
  Serial.print(kSweepStepMs);
  Serial.print("ms for ");
  Serial.print(kSweepWindowMs);
  Serial.println("ms");
  Serial.print("  resolution is one probe pass, so a reported ");
  Serial.print(kSweepStepMs);
  Serial.println("ms or less means it answered on the first attempt");

  int32_t best[kProbeCount];
  int32_t worst[kProbeCount];
  uint8_t misses[kProbeCount];
  for (uint8_t i = 0; i < kProbeCount; ++i) {
    best[i] = INT32_MAX;
    worst[i] = -1;
    misses[i] = 0;
  }

  for (uint8_t round = 0; round < kSweepRounds; ++round) {
    railDown();
    delay(kRailOffMs);

    const uint32_t t0 = millis();
    power.enable(static_cast<uint64_t>(t0));
    busUp();

    int32_t firstAck[kProbeCount];
    for (uint8_t i = 0; i < kProbeCount; ++i) {
      firstAck[i] = -1;
    }
    while (millis() - t0 < kSweepWindowMs) {
      for (uint8_t i = 0; i < kProbeCount; ++i) {
        if (firstAck[i] < 0 && acknowledges(kProbes[i].address)) {
          firstAck[i] = static_cast<int32_t>(millis() - t0);
        }
      }
      delay(kSweepStepMs);
    }

    Serial.print("  round ");
    Serial.print(round + 1);
    for (uint8_t i = 0; i < kProbeCount; ++i) {
      Serial.print("  ");
      Serial.print(kProbes[i].name);
      Serial.print("=");
      if (firstAck[i] < 0) {
        Serial.print("none");
        misses[i]++;
      } else {
        Serial.print(firstAck[i]);
        Serial.print("ms");
        if (firstAck[i] < best[i]) {
          best[i] = firstAck[i];
        }
        if (firstAck[i] > worst[i]) {
          worst[i] = firstAck[i];
        }
      }
    }
    Serial.println();
  }

  railDown();
  Serial.println("  summary (first ACK after rail-on):");
  for (uint8_t i = 0; i < kProbeCount; ++i) {
    Serial.print("    ");
    Serial.print(kProbes[i].name);
    if (worst[i] < 0) {
      Serial.println("  never answered");
      continue;
    }
    Serial.print("  best=");
    Serial.print(best[i]);
    Serial.print("ms worst=");
    Serial.print(worst[i]);
    Serial.print("ms misses=");
    Serial.print(misses[i]);
    Serial.print("/");
    Serial.print(kSweepRounds);
    if (misses[i] > 0 ||
        worst[i] >= static_cast<int32_t>(PowerDomain::kSettleMs)) {
      Serial.print("  <-- LATER THAN PowerDomain::kSettleMs, firmware probes too early");
    }
    Serial.println();
  }
}

void runBusScan() {
  Serial.println();
  Serial.print("I2C scan (settle=");
  Serial.print(settleMs);
  Serial.println("ms, rail left on):");
  railDown();
  delay(kRailOffMs);
  railUpAndSettle();
  uint8_t found = 0;
  for (uint8_t address = 0x08; address < 0x78; ++address) {
    if (!acknowledges(address)) {
      continue;
    }
    found++;
    Serial.print("  0x");
    Serial.print(address, HEX);
    for (uint8_t i = 0; i < kProbeCount; ++i) {
      if (kProbes[i].address == address) {
        Serial.print("  ");
        Serial.print(kProbes[i].name);
      }
    }
    Serial.println();
  }
  if (found == 0) {
    Serial.println("  nothing answered -- suspect the rail before the bus (HARDWARE.md §7)");
  }
}

// Anything that ACKs with the rail down is being fed through its ESD diodes.
//
// Only a positive result is conclusive. The probe drives SDA/SCL itself and
// supplies its own pull-ups, so an ACK proves the parasitic path exists rather
// than that the part was already awake -- but the path existing is the point:
// it means the sensors never see a clean power-on reset between cycles.
// Silence proves nothing at all, since an unpowered part cannot answer either
// way. Confirm any suspicion with a meter on C20 (HARDWARE.md §2).
void runBackPowerProbe() {
  Serial.println();
  Serial.println("back-power probe: rail off, internal pull-ups, then re-probe");
  railDown();
  delay(kRailOffMs);
  busUp();
  enableInternalPullup(kSdaPortPin);
  enableInternalPullup(kSclPortPin);

  uint8_t answered = 0;
  for (uint8_t i = 0; i < kProbeCount; ++i) {
    if (acknowledges(kProbes[i].address)) {
      answered++;
      Serial.print("  ");
      Serial.print(kProbes[i].name);
      Serial.println(" ANSWERED with the rail off");
    }
  }
  if (answered == 0) {
    Serial.println("  silent -- inconclusive, not proof of a clean rail");
  } else {
    Serial.println("  parasitic path confirmed; measure +3.3V at C20 with the rail off");
  }
  busDown();
}

// The SCD4x does not ACK its address while in power-down, and its wake_up
// command is itself unacknowledged by design. That state should not survive a
// rail cycle, so this only matters if the rail is not really collapsing --
// which is exactly the doubt the back-power probe leaves open.
void runScd41WakeProbe() {
  Serial.println();
  Serial.println("SCD41 wake probe:");
  railDown();
  delay(kRailOffMs);
  railUpAndSettle();
  Serial.print("  before wake: ");
  Serial.println(acknowledges(0x62) ? "ACK" : "no ACK");

  SensirionI2cScd4x device;
  device.begin(Wire, SCD41_I2C_ADDR_62);
  device.wakeUp();
  delay(30);  // datasheet: 30 ms from wake_up to idle

  Serial.print("  after wake:  ");
  Serial.println(acknowledges(0x62) ? "ACK -- it was in power-down" : "no ACK");
  railDown();
}

// Live monitor: rail held on, every address re-probed continuously, a line
// printed whenever anything changes state. Counts transitions per address.
//
// This is the test for a joint that has cracked since the board was tested --
// press or gently flex U6 with a plastic tool while it runs. A single
// transition is proof of intermittent contact, which no static probe can
// distinguish from a dead part. Runs until a key arrives.
void runMonitor() {
  Serial.println();
  Serial.println("monitor: rail held on, probing continuously.");
  Serial.println("  press/flex U6 with a plastic tool -- any transition means a bad joint");
  Serial.println("  press any key to stop");
  railDown();
  delay(kRailOffMs);
  railUpAndSettle();

  bool state[kProbeCount];
  uint16_t transitions[kProbeCount];
  for (uint8_t i = 0; i < kProbeCount; ++i) {
    state[i] = acknowledges(kProbes[i].address);
    transitions[i] = 0;
  }

  const uint32_t startMs = millis();
  uint32_t lastPrintMs = 0;
  bool changed = true;  // force the opening line
  while (Serial.available() == 0) {
    for (uint8_t i = 0; i < kProbeCount; ++i) {
      const bool now = acknowledges(kProbes[i].address);
      if (now != state[i]) {
        state[i] = now;
        if (transitions[i] < UINT16_MAX) {
          transitions[i]++;
        }
        changed = true;
      }
    }
    if (changed || millis() - lastPrintMs >= kMonitorHeartbeatMs) {
      lastPrintMs = millis();
      changed = false;
      Serial.print("  t=");
      Serial.print((millis() - startMs) / 1000);
      Serial.print("s ");
      for (uint8_t i = 0; i < kProbeCount; ++i) {
        Serial.print(" ");
        Serial.print(kProbes[i].name);
        Serial.print(state[i] ? "=ACK" : "=---");
      }
      Serial.println();
    }
    delay(kMonitorStepMs);
  }
  while (Serial.available() > 0) {
    Serial.read();  // swallow the stop key so it is not read as a command
  }

  Serial.println("  transitions:");
  for (uint8_t i = 0; i < kProbeCount; ++i) {
    Serial.print("    ");
    Serial.print(kProbes[i].name);
    Serial.print(" = ");
    Serial.print(transitions[i]);
    Serial.println(transitions[i] > 0 ? "  <-- INTERMITTENT, this is a joint" : "");
  }
  railDown();
}

// A joint that is resistive rather than fully open can still pass at a slower
// clock, where the longer bit period tolerates a degraded rise time. An ACK at
// 10 kHz that is absent at 100 kHz is conclusive evidence of a bad contact
// rather than a dead part. Costs nothing to try.
void runBusSpeedSweep() {
  static const uint32_t kSpeeds[] = {400000, 100000, 50000, 10000};
  Serial.println();
  Serial.println("bus speed sweep (400 kHz is out of spec for the SCD41, shown anyway):");
  railDown();
  delay(kRailOffMs);
  power.enable(nowMs());
  delay(settleMs);

  for (uint8_t s = 0; s < sizeof(kSpeeds) / sizeof(kSpeeds[0]); ++s) {
    busUp(kSpeeds[s]);
    Serial.print("  ");
    Serial.print(kSpeeds[s] / 1000);
    Serial.print("kHz ");
    for (uint8_t i = 0; i < kProbeCount; ++i) {
      Serial.print(" ");
      Serial.print(kProbes[i].name);
      Serial.print(acknowledges(kProbes[i].address) ? "=ACK" : "=---");
    }
    Serial.println();
  }
  railDown();
}

// Reads the settings that survive power loss. ASC in particular: it is enabled
// from the factory and needs days of continuous running to build a baseline,
// which a rail cycled every 60-300 s never gives it.
void runScd41Dump() {
  Serial.println();
  Serial.println("SCD41 persistent settings:");
  railDown();
  delay(kRailOffMs);
  railUpAndSettle();

  if (!acknowledges(0x62)) {
    Serial.println("  no ACK at 0x62 -- raise settle with '+' and retry");
    railDown();
    return;
  }

  SensirionI2cScd4x device;
  device.begin(Wire, SCD41_I2C_ADDR_62);
  device.stopPeriodicMeasurement();
  delay(500);  // datasheet: commands are ignored for 500 ms after stop

  uint64_t serialNumber = 0;
  if (device.getSerialNumber(serialNumber) == 0) {
    Serial.print("  serial=0x");
    Serial.print(static_cast<uint32_t>(serialNumber >> 32), HEX);
    Serial.println(static_cast<uint32_t>(serialNumber), HEX);
  } else {
    Serial.println("  serial=read failed");
  }

  uint16_t asc = 0;
  if (device.getAutomaticSelfCalibrationEnabled(asc) == 0) {
    Serial.print("  ASC=");
    Serial.print(asc);
    Serial.println(asc ? "  <-- enabled, but this device is power cycled" : "");
  } else {
    Serial.println("  ASC=read failed");
  }

  float temperatureOffset = 0.0f;
  if (device.getTemperatureOffset(temperatureOffset) == 0) {
    Serial.print("  tempOffset=");
    Serial.print(temperatureOffset, 2);
    Serial.println("C");
  } else {
    Serial.println("  tempOffset=read failed");
  }

  uint16_t altitude = 0;
  if (device.getSensorAltitude(altitude) == 0) {
    Serial.print("  altitude=");
    Serial.print(altitude);
    Serial.println("m");
  } else {
    Serial.println("  altitude=read failed");
  }

  uint32_t pressure = 0;
  if (device.getAmbientPressure(pressure) == 0) {
    Serial.print("  ambientPressure=");
    Serial.print(pressure);
    Serial.println("Pa");
  } else {
    Serial.println("  ambientPressure=read failed");
  }

  railDown();
}

bool inRange(float value, float lo, float hi) {
  return value >= lo && value <= hi;
}

void printVerdict(const char* name, bool ok, bool missing, const char* hint) {
  Serial.print("  ");
  Serial.print(name);
  if (ok) {
    Serial.println("PASS");
  } else if (missing) {
    Serial.print("FAIL missing -- ");
    Serial.println(hint);
  } else {
    Serial.println("FAIL implausible or invalid reading (see line above)");
  }
}

void reportCycle() {
  const bool bmePresent = bme280.present();
  const bool vemlPresent = veml7700.present();
  const bool scdPresent = scd41.present();
  const bool bmeTimedOut = bme280.timedOut();
  const bool vemlTimedOut = veml7700.timedOut();
  const bool scdTimedOut = scd41.timedOut();

  const bool micPresent = microphone.present();
  const bool micTimedOut = microphone.timedOut();

  bme280.collect(reading);
  veml7700.collect(reading);
  scd41.collect(reading);
  microphone.collect(reading);

  bool batteryOk = false;
  if (enableBattery) {
    float volts = 0.0f;
    batteryOk = batteryMonitor.sample(volts);
    if (batteryOk) {
      reading.batteryV = volts;
      reading.valid |= VALID_BATTERY;
    }
  }

  constexpr uint32_t kBmeReadings =
      VALID_TEMPERATURE | VALID_HUMIDITY | VALID_PRESSURE;

  Serial.print("cycle=");
  Serial.print(cycleNumber);
  Serial.print(" settle=");
  Serial.print(settleMs);
  printValue("ms T=", reading.temperatureC, reading.valid & VALID_TEMPERATURE,
             2);
  printValue("C RH=", reading.humidityPct, reading.valid & VALID_HUMIDITY, 1);
  printValue("% P=", reading.pressurePa / 100.0f,
             reading.valid & VALID_PRESSURE, 1);
  printValue("hPa CO2=", reading.co2Ppm, reading.valid & VALID_CO2, 0);
  printValue("ppm lux=", reading.lux, reading.valid & VALID_LIGHT, 1);
  printValue(" noise=", reading.noiseDb, reading.valid & VALID_NOISE, 1);
  printValue("dB batt=", reading.batteryV, reading.valid & VALID_BATTERY, 2);
  Serial.println(batteryMonitor.pollCharging(nowMs()) ? "V charging" : "V");

  printState("        bme=", bmePresent, bmeTimedOut,
             (reading.valid & kBmeReadings) == kBmeReadings);
  printState(" veml=", vemlPresent, vemlTimedOut, reading.valid & VALID_LIGHT);
  printState(" scd=", scdPresent, scdTimedOut, reading.valid & VALID_CO2);
  printState(" mic=", micPresent, micTimedOut, reading.valid & VALID_NOISE);
  printState(" batt=", enableBattery, false, batteryOk);

  printLatency("  |  tBme=", bmeDoneMs);
  printLatency(" tVeml=", vemlDoneMs);
  printLatency(" tScd=", scdDoneMs);
  Serial.print("/");
  Serial.print(2 * Scd41Sensor::kSingleShotTimeoutMs);  // discarded + measured
  printLatency("ms tMic=", micDoneMs);
  Serial.println();

  // Raw acoustic statistics: dB SPL alone cannot be checked against anything,
  // but these are exactly what HARDWARE.md's known-good table is written in.
  // Quiet room rms 20-120, peak < 250, dc ~ 0, clip 0. A large rms with clip
  // > 0 and a big dc is the stopped-clock wake-up transient, not audio.
  if (micPresent) {
    const AcousticMetrics& m = microphone.metrics();
    Serial.print("        mic rms=");
    Serial.print(m.rms(), 1);
    Serial.print(" peak=");
    Serial.print(m.peak());
    Serial.print(" dc=");
    Serial.print(m.dcMean());
    Serial.print(" clip=");
    Serial.print(m.clipped());
    Serial.print(" buffers=");
    Serial.println(m.buffers());
  }

  if (unitTest) {
    unitTest = false;
    cycling = false;
    const bool bmeOk = (reading.valid & kBmeReadings) == kBmeReadings &&
                       inRange(reading.temperatureC, 0.0f, 50.0f) &&
                       inRange(reading.humidityPct, 5.0f, 95.0f) &&
                       inRange(reading.pressurePa / 100.0f, 800.0f, 1100.0f);
    const bool vemlOk = (reading.valid & VALID_LIGHT) && reading.lux >= 0.0f;
    const bool scdOk = (reading.valid & VALID_CO2) &&
                       inRange(reading.co2Ppm, 300.0f, 5000.0f);
    const bool micOk = (reading.valid & VALID_NOISE) &&
                       microphone.metrics().clipped() == 0 &&
                       inRange(reading.noiseDb, 20.0f, 110.0f);
    const bool battOk = (reading.valid & VALID_BATTERY) &&
                        inRange(reading.batteryV, 3.0f, 4.35f);

    Serial.println();
    Serial.println("unit test:");
    printVerdict("BME280   ", bmeOk, !bmePresent,
                 "0x76 silent: rail, SDA/SCL, or U2 joints (HARDWARE.md §7)");
    printVerdict("VEML7700 ", vemlOk, !vemlPresent,
                 "0x10 silent: rail, SDA/SCL, or U5 joints");
    printVerdict("SCD41    ", scdOk, !scdPresent,
                 "no reply to get_serial_number: run 'x', then meter C20");
    printVerdict("mic      ", micOk, !micPresent,
                 "no PDM data: rail, PIN_CNF, or U4 (HARDWARE.md §4)");
    printVerdict("battery  ", battOk, !batteryOk,
                 "SAADC read failed or no cell on J2");
    if (scdPresent && !scdOk && scdTimedOut) {
      Serial.println("  SCD41 answered but no result in 7 s: suspect supply sag");
    }
    sensorsPassed = bmeOk && vemlOk && scdOk && micOk && battOk;
    boardChecksPending = true;
  }
}

// ---- board checks: flash, display, BLE ----
//
// Each returns nullptr on a pass or a one-line hint naming the failed step.
// Flash and display need only the rail (they share SPI), not the I2C bus.

void railUpForSpi() {
  railDown();
  power.enable(nowMs());
  delay(settleMs);
}

// Every page gets a pattern that depends on its address, so a stuck or
// aliased address line shows as a mismatch on the final read-back rather than
// passing because every page held the same bytes.
uint8_t flashPattern(uint32_t address) {
  return static_cast<uint8_t>(address ^ (address >> 8) ^ 0x5A);
}

const char* checkFlashOnRail() {
  static char hint[64];
  if (!flash.begin()) {
    return "JEDEC ID is not EF 40 17: rail, SPI lines or U1 joints";
  }
  if (!flash.eraseSector(kTestSector)) {
    return "sector erase timed out";
  }
  uint8_t page[W25Q64Flash::kPageBytes];
  for (uint32_t offset = 0; offset < W25Q64Flash::kSectorBytes;
       offset += sizeof page) {
    if (!flash.read(kTestSector + offset, page, sizeof page)) {
      return "read refused";
    }
    for (uint32_t i = 0; i < sizeof page; ++i) {
      if (page[i] != 0xFF) {
        snprintf(hint, sizeof hint, "not blank after erase at 0x%06lX",
                 static_cast<unsigned long>(kTestSector + offset + i));
        return hint;
      }
    }
  }
  for (uint32_t offset = 0; offset < W25Q64Flash::kSectorBytes;
       offset += sizeof page) {
    for (uint32_t i = 0; i < sizeof page; ++i) {
      page[i] = flashPattern(kTestSector + offset + i);
    }
    if (!flash.program(kTestSector + offset, page, sizeof page)) {
      snprintf(hint, sizeof hint, "program/verify failed at 0x%06lX",
               static_cast<unsigned long>(kTestSector + offset));
      return hint;
    }
  }
  // program() verified each page as it went; reading the whole sector again
  // catches a later page overwriting an earlier one.
  for (uint32_t offset = 0; offset < W25Q64Flash::kSectorBytes;
       offset += sizeof page) {
    if (!flash.read(kTestSector + offset, page, sizeof page)) {
      return "read refused";
    }
    for (uint32_t i = 0; i < sizeof page; ++i) {
      if (page[i] != flashPattern(kTestSector + offset + i)) {
        snprintf(hint, sizeof hint, "read-back mismatch at 0x%06lX",
                 static_cast<unsigned long>(kTestSector + offset + i));
        return hint;
      }
    }
  }
  if (!flash.eraseSector(kTestSector)) {  // leave the sector blank
    return "final erase timed out";
  }
  return nullptr;
}

const char* checkFlash() {
  railUpForSpi();
  const char* failure = checkFlashOnRail();
  flash.end();
  railDown();
  return failure;
}

// Checkerboard for stuck or ghosting pixels, a one-pixel border for the edge
// rows and columns, text for the font path, and a solid bar for even ink.
void drawTestPattern() {
  using Ink = EpaperDisplay::Ink;
  constexpr int16_t kW = EpaperDisplay::kWidth;
  constexpr int16_t kH = EpaperDisplay::kHeight;
  constexpr int16_t kCell = kW / 8;
  display.beginFrame();
  for (int16_t row = 0; row < 4; ++row) {
    for (int16_t col = 0; col < 8; ++col) {
      if ((row + col) & 1) {
        display.fillRect(col * kCell, row * kCell, kCell, kCell, Ink::kBlack);
      }
    }
  }
  display.drawFastHLine(0, 0, kW, Ink::kBlack);
  display.drawFastHLine(0, kH - 1, kW, Ink::kBlack);
  display.drawFastVLine(0, 0, kH, Ink::kBlack);
  display.drawFastVLine(kW - 1, 0, kH, Ink::kBlack);

  display.setTextColor(Ink::kBlack);
  display.setFont(EpaperDisplay::Font::kSemiBold12);
  const char* title = "TEST";
  display.setCursor((kW - display.textWidth(title)) / 2, 4 * kCell + 26);
  display.print(title);
  display.setFont(EpaperDisplay::Font::kRegular9);
  const char* suffix =
      unitSerial + UnitIdentity::kSerialChars - UnitIdentity::kNameSuffixChars;
  display.setCursor((kW - display.textWidth(suffix)) / 2, 4 * kCell + 48);
  display.print(suffix);
  display.fillRect(0, kH - 12, kW, 12, Ink::kBlack);
}

const char* checkDisplay(uint32_t& refreshMs) {
  refreshMs = 0;
  railUpForSpi();
  const char* failure = nullptr;
  if (!display.begin(true)) {
    failure = "BUSY held low after reset: rail, ED_RES or J1";
  } else {
    drawTestPattern();
    const uint32_t startMs = millis();
    const bool refreshed = display.endFrame();
    refreshMs = millis() - startMs;
    if (!refreshed) {
      failure = display.timedOut()
                    ? "BUSY never released: rail or gate-voltage boost"
                    : "BUSY never went low: panel silent, check ED_DC, ED_CS, J1";
    }
  }
  const bool hibernated = display.end();
  railDown();
  if (failure == nullptr && !hibernated) {
    failure = "BUSY held low after hibernate";
  }
  return failure;
}

// The radio is inside the certified module, so a stack that starts and
// accepts an advertising set is the automatic verdict. Finding the name on a
// phone proves the air path; a connection is reported as it happens.
const char* checkBle() {
  if (!bleStarted) {
    if (!BLE.begin()) {
      return "BLE.begin() failed: the stack did not start";
    }
    bleStarted = true;
  }
  BLE.stopAdvertise();
  BLE.setLocalName(bleName);
  if (!BLE.advertise()) {
    return "advertise() refused";
  }
  bleAdvertising = true;
  bleAdvertiseUntilMs = millis() + kBleAdvertiseMs;
  return nullptr;
}

void serviceBle() {
  if (!bleStarted) {
    return;
  }
  BLE.poll();
  const bool connected = BLE.connected();
  if (connected != bleWasConnected) {
    bleWasConnected = connected;
    Serial.println(connected ? "BLE: central connected -- radio OK both ways"
                             : "BLE: central disconnected");
  }
  if (bleAdvertising && !connected &&
      static_cast<int32_t>(millis() - bleAdvertiseUntilMs) >= 0) {
    BLE.stopAdvertise();
    bleAdvertising = false;
    Serial.println("BLE: advertising stopped");
  }
}

void printBoardVerdict(const char* name, const char* failure) {
  Serial.print("  ");
  Serial.print(name);
  if (failure == nullptr) {
    Serial.println("PASS");
  } else {
    Serial.print("FAIL ");
    Serial.println(failure);
  }
}

void runFlashCheck() {
  Serial.println();
  Serial.print("flash check (test sector 0x");
  Serial.print(kTestSector, HEX);
  Serial.println(" only):");
  printBoardVerdict("flash    ", checkFlash());
}

void runDisplayCheck() {
  Serial.println();
  Serial.println("display check (full refresh, test pattern):");
  uint32_t refreshMs = 0;
  const char* failure = checkDisplay(refreshMs);
  printBoardVerdict("display  ", failure);
  Serial.print("  refresh=");
  Serial.print(refreshMs);
  Serial.println("ms -- panel must show a checkerboard, border, TEST, the");
  Serial.println("  last four serial digits and a black bar, with no grey patches");
}

void printBleAdvertising() {
  Serial.print("  advertising as \"");
  Serial.print(bleName);
  Serial.print("\" for ");
  Serial.print(kBleAdvertiseMs / 1000);
  Serial.println(" s: find it in nRF Connect");
}

void runBleCheck() {
  Serial.println();
  Serial.println("BLE check:");
  const char* failure = checkBle();
  printBoardVerdict("BLE      ", failure);
  if (failure == nullptr) {
    printBleAdvertising();
  }
}

// Runs after the sensor verdicts, with the rail already down.
void finishUnitTest() {
  const char* flashFailure = checkFlash();
  uint32_t refreshMs = 0;
  const char* displayFailure = checkDisplay(refreshMs);
  const char* bleFailure = checkBle();

  printBoardVerdict("flash    ", flashFailure);
  printBoardVerdict("display  ", displayFailure);
  printBoardVerdict("BLE      ", bleFailure);
  Serial.print("  serial=");
  Serial.print(unitSerial);
  Serial.print(" display refresh=");
  Serial.print(refreshMs);
  Serial.println("ms");
  Serial.println("  look at the panel: checkerboard, border, TEST, serial digits, black bar");
  if (bleFailure == nullptr) {
    printBleAdvertising();
  }
  const bool pass = sensorsPassed && flashFailure == nullptr &&
                    displayFailure == nullptr && bleFailure == nullptr;
  Serial.println(pass ? "RESULT: PASS" : "RESULT: FAIL");
}

void serviceCycle() {
  const uint64_t now = nowMs();

  switch (phase) {
    case Phase::kIdle:
      if (!cycling) {
        return;
      }
      ++cycleNumber;
      railOnAtMs = millis();
      power.enable(now);
      phase = Phase::kSettling;
      break;

    case Phase::kSettling:
      if (millis() - railOnAtMs >= settleMs) {
        busUp();
        phase = Phase::kInitializing;
      }
      break;

    case Phase::kInitializing:
      if (enableBme280) {
        bme280.begin();
      }
      if (enableVeml7700) {
        veml7700.begin();
      }
      if (enableScd41) {
        scd41.begin();
      }
      if (enableMicrophone) {
        microphone.begin();
      }
      phase = Phase::kAcquiring;
      break;

    case Phase::kAcquiring:
      reading = Reading{};
      reading.monotonicMs = now;
      bme280.start(now);
      veml7700.start(now);
      scd41.start(now);
      microphone.start(now);
      acquireAtMs = millis();
      bmeDoneMs = bme280.present() ? -1 : -2;
      vemlDoneMs = veml7700.present() ? -1 : -2;
      scdDoneMs = scd41.present() ? -1 : -2;
      micDoneMs = microphone.present() ? -1 : -2;
      phase = Phase::kWaiting;
      break;

    case Phase::kWaiting: {
      bme280.poll(now);
      veml7700.poll(now);
      scd41.poll(now);
      microphone.poll(now);
      const int32_t elapsed = static_cast<int32_t>(millis() - acquireAtMs);
      if (bmeDoneMs == -1 && bme280.ready()) {
        bmeDoneMs = elapsed;
      }
      if (vemlDoneMs == -1 && veml7700.ready()) {
        vemlDoneMs = elapsed;
      }
      if (scdDoneMs == -1 && scd41.ready()) {
        scdDoneMs = elapsed;
      }
      if (micDoneMs == -1 && microphone.ready()) {
        micDoneMs = elapsed;
      }
      if (bme280.ready() && veml7700.ready() && scd41.ready() &&
          microphone.ready()) {
        phase = Phase::kReporting;
      }
      break;
    }

    case Phase::kReporting:
      reportCycle();
      bme280.end();
      veml7700.end();
      scd41.end();
      microphone.end();
      railDown();
      if (boardChecksPending) {
        boardChecksPending = false;
        finishUnitTest();
      }
      cooldownUntilMs = millis() + kCycleGapMs;
      phase = Phase::kCooldown;
      break;

    case Phase::kCooldown:
      if (static_cast<int32_t>(millis() - cooldownUntilMs) >= 0) {
        phase = Phase::kIdle;
      }
      break;
  }
}

// The one-shot commands all drive the rail themselves, so a cycle caught
// mid-flight has to be torn down first -- otherwise the drivers keep their
// kMeasuring state across a rail cut and the next 'c' starts from a lie.
void abortCycle() {
  cycling = false;
  unitTest = false;
  boardChecksPending = false;
  if (phase == Phase::kIdle) {
    return;
  }
  bme280.end();
  veml7700.end();
  scd41.end();
  microphone.end();
  railDown();
  phase = Phase::kIdle;
}

// ---- 'x': exhaustive SCD41 probe ----
//
// Everything software can still try before a meter goes on C20. The bit-bang
// half drives P0.04/P0.05 straight from the port registers, so the nRF's TWIM
// peripheral and the mbed Wire layer are both out of the picture: an ACK here
// and not through Wire would be a firmware bug, silence in both is hardware.
// S0D1 makes the pins open-drain, so OUTSET releases a line to the external
// pull-ups and OUTCLR pulls it low -- the same electrical contract as I2C.

constexpr uint32_t kBbHalfUs = 100;           // ~5 kHz, far below any limit
constexpr uint32_t kBbStretchLimitUs = 50000;  // SCD41 never stretches this long

void bbConfigure(uint32_t portPin) {
  NRF_P0->OUTSET = 1u << portPin;
  NRF_P0->PIN_CNF[portPin] =
      (GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos) |
      (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos) |
      (GPIO_PIN_CNF_PULL_Disabled << GPIO_PIN_CNF_PULL_Pos) |
      (GPIO_PIN_CNF_DRIVE_S0D1 << GPIO_PIN_CNF_DRIVE_Pos) |
      (GPIO_PIN_CNF_SENSE_Disabled << GPIO_PIN_CNF_SENSE_Pos);
}

void bbSet(uint32_t portPin, bool high) {
  if (high) {
    NRF_P0->OUTSET = 1u << portPin;
  } else {
    NRF_P0->OUTCLR = 1u << portPin;
  }
}

bool bbRead(uint32_t portPin) {
  return (NRF_P0->IN >> portPin) & 1u;
}

void bbHalf() {
  delayMicroseconds(kBbHalfUs);
}

// Releases SCL and honours clock stretching; false means SCL is held low.
bool bbSclHigh() {
  bbSet(kSclPortPin, true);
  const uint32_t t0 = micros();
  while (!bbRead(kSclPortPin)) {
    if (micros() - t0 > kBbStretchLimitUs) {
      return false;
    }
  }
  return true;
}

void bbStart() {
  bbSet(kSdaPortPin, true);
  bbSclHigh();
  bbHalf();
  bbSet(kSdaPortPin, false);
  bbHalf();
  bbSet(kSclPortPin, false);
  bbHalf();
}

void bbStop() {
  bbSet(kSdaPortPin, false);
  bbHalf();
  bbSclHigh();
  bbHalf();
  bbSet(kSdaPortPin, true);
  bbHalf();
}

bool bbWriteByte(uint8_t value) {
  for (int8_t bit = 7; bit >= 0; --bit) {
    bbSet(kSdaPortPin, (value >> bit) & 1u);
    bbHalf();
    bbSclHigh();
    bbHalf();
    bbSet(kSclPortPin, false);
  }
  bbSet(kSdaPortPin, true);  // release for the ACK slot
  bbHalf();
  bbSclHigh();
  const bool ack = !bbRead(kSdaPortPin);
  bbHalf();
  bbSet(kSclPortPin, false);
  return ack;
}

uint8_t bbReadByte(bool ack) {
  uint8_t value = 0;
  bbSet(kSdaPortPin, true);
  for (uint8_t i = 0; i < 8; ++i) {
    bbHalf();
    bbSclHigh();
    value = static_cast<uint8_t>((value << 1) | (bbRead(kSdaPortPin) ? 1 : 0));
    bbHalf();
    bbSet(kSclPortPin, false);
  }
  bbSet(kSdaPortPin, !ack);
  bbHalf();
  bbSclHigh();
  bbHalf();
  bbSet(kSclPortPin, false);
  bbSet(kSdaPortPin, true);
  return value;
}

bool bbProbe(uint8_t address, bool read) {
  bbStart();
  const bool ack = bbWriteByte(static_cast<uint8_t>((address << 1) | read));
  if (ack && read) {
    bbReadByte(false);  // a slave that ACKed a read owns SDA until NACKed
  }
  bbStop();
  return ack;
}

// Nine clocks with SDA released frees a slave stuck mid-byte.
void bbRecover() {
  bbSet(kSdaPortPin, true);
  for (uint8_t i = 0; i < 9; ++i) {
    bbSet(kSclPortPin, false);
    bbHalf();
    bbSclHigh();
    bbHalf();
  }
  bbStop();
}

void bbBegin() {
  busDown();  // TWIM must let go of the pins first
  bbConfigure(kSdaPortPin);
  bbConfigure(kSclPortPin);
  bbHalf();
}

void printAck(const char* label, bool ack) {
  Serial.print(label);
  Serial.println(ack ? "ACK" : "no ACK");
}

// SDA/SCL idle levels and cross-talk: a bridge between them, or a line held
// by U6's pad, shows here without any protocol involved.
void bbLineCheck() {
  Serial.print("  idle: SDA=");
  Serial.print(bbRead(kSdaPortPin));
  Serial.print(" SCL=");
  Serial.println(bbRead(kSclPortPin));

  bbSet(kSclPortPin, false);
  delayMicroseconds(20);
  const bool sdaWhileSclLow = bbRead(kSdaPortPin);
  bbSet(kSclPortPin, true);
  bbSet(kSdaPortPin, false);
  delayMicroseconds(20);
  const bool sclWhileSdaLow = bbRead(kSclPortPin);
  const bool sdaFollows = !bbRead(kSdaPortPin);
  bbSet(kSdaPortPin, true);
  delayMicroseconds(20);
  Serial.print("  SCL low -> SDA=");
  Serial.print(sdaWhileSclLow);
  Serial.print("   SDA low -> SCL=");
  Serial.print(sclWhileSdaLow);
  Serial.print(" (SDA drive ");
  Serial.print(sdaFollows ? "ok" : "FAILED");
  Serial.println(")");
  if (!sdaWhileSclLow || !sclWhileSdaLow) {
    Serial.println("  -> SDA and SCL are bridged somewhere");
  }
}

void bbScan() {
  Serial.print("  ");
  uint8_t found = 0;
  for (uint8_t address = 0x08; address <= 0x77; ++address) {
    if (bbProbe(address, false)) {
      Serial.print("0x");
      Serial.print(address, HEX);
      Serial.print(" ");
      found++;
    }
  }
  if (found == 0) {
    Serial.print("nothing");
  }
  Serial.println();
}

void printScd41Error(const char* label, int16_t error) {
  Serial.print(label);
  if (error == 0) {
    Serial.println("ok");
  } else {
    char message[64];
    errorToString(error, message, sizeof message);
    Serial.print("error ");
    Serial.print(error);
    Serial.print(" (");
    Serial.print(message);
    Serial.println(")");
  }
}

void runScd41Exhaustive() {
  Serial.println();
  Serial.println("exhaustive SCD41 probe (~60 s)");

  Serial.println("[1] rail off 15 s (full discharge, R7 bleeder is NC), then first ACK");
  railDown();
  delay(15000);
  power.enable(nowMs());
  bbBegin();
  const uint32_t railOnMs = millis();
  int32_t firstAckMs = -1;
  while (millis() - railOnMs < 5000) {
    if (bbProbe(0x62, false)) {
      firstAckMs = static_cast<int32_t>(millis() - railOnMs);
      break;
    }
    delay(5);
  }
  Serial.print("  0x62 first ACK: ");
  if (firstAckMs < 0) {
    Serial.println("none in 5 s");
  } else {
    Serial.print(firstAckMs);
    Serial.println(" ms");
  }

  Serial.println("[2] line check (bit-bang, TWIM detached)");
  bbLineCheck();

  Serial.println("[3] bit-bang scan 0x08-0x77 at ~5 kHz");
  bbRecover();
  bbScan();
  printAck("  0x62 read-address: ", bbProbe(0x62, true));

  Serial.println("[4] I2C general-call reset (0x00, 0x06)");
  bbStart();
  const bool gcAck = bbWriteByte(0x00);
  const bool gcCmdAck = gcAck && bbWriteByte(0x06);
  bbStop();
  Serial.print("  general call ");
  Serial.print(gcAck ? "ACKed" : "not ACKed");
  Serial.println(gcAck ? (gcCmdAck ? ", reset ACKed" : ", reset NAKed") : "");
  delay(30);
  printAck("  0x62 after reset: ", bbProbe(0x62, false));

  Serial.println("[5] wake_up 0x36F6 (unacknowledged by design), then 0x62");
  bbStart();
  bbWriteByte(0x62 << 1);
  bbWriteByte(0x36);
  bbWriteByte(0xF6);
  bbStop();
  delay(30);
  printAck("  0x62 after wake: ", bbProbe(0x62, false));

  Serial.println("[6] 0x62 every 250 ms for 20 s, rail held on");
  uint16_t acks = 0;
  for (uint8_t i = 0; i < 80; ++i) {
    if (bbProbe(0x62, false)) {
      acks++;
    }
    delay(250);
  }
  Serial.print("  ");
  Serial.print(acks);
  Serial.println("/80 ACKed");

  Serial.println("[7] Sensirion driver over Wire: wake, stop, reinit, serial");
  busUp();
  SensirionI2cScd4x device;
  device.begin(Wire, SCD41_I2C_ADDR_62);
  device.wakeUp();
  delay(30);
  printScd41Error("  stopPeriodicMeasurement: ", device.stopPeriodicMeasurement());
  delay(500);
  printScd41Error("  reinit: ", device.reinit());
  delay(30);
  uint64_t serial = 0;
  const int16_t serialError = device.getSerialNumber(serial);
  printScd41Error("  getSerialNumber: ", serialError);
  if (serialError == 0) {
    Serial.print("  serial=0x");
    Serial.print(static_cast<uint32_t>(serial >> 32), HEX);
    Serial.println(static_cast<uint32_t>(serial), HEX);
  }

  railDown();
  Serial.println("done");
}

void handleCommand(char command) {
  switch (command) {
    case '?':
      printHelp();
      break;
    case 'x':
      abortCycle();
      runScd41Exhaustive();
      break;
    case 's':
      abortCycle();
      runSettleSweep();
      break;
    case 'i':
      abortCycle();
      runBusScan();
      break;
    case 'b':
      abortCycle();
      runBackPowerProbe();
      break;
    case 'w':
      abortCycle();
      runScd41WakeProbe();
      break;
    case 'm':
      abortCycle();
      runMonitor();
      break;
    case 'f':
      abortCycle();
      runBusSpeedSweep();
      break;
    case 'd':
      abortCycle();
      runScd41Dump();
      break;
    case 'n':
      abortCycle();
      runFlashCheck();
      break;
    case 'e':
      abortCycle();
      runDisplayCheck();
      break;
    case 'a':
      runBleCheck();
      break;
    case 't':
      abortCycle();
      enableBme280 = enableVeml7700 = enableScd41 = true;
      enableMicrophone = enableBattery = true;
      Serial.println("unit test: one measurement cycle, then flash, display, BLE (~20 s)...");
      unitTest = true;
      cycling = true;
      break;
    case 'c':
      cycling = !cycling;
      Serial.println(cycling ? "cycling on" : "cycling off (finishing cycle)");
      break;
    case '1':
      enableBme280 = !enableBme280;
      Serial.println(enableBme280 ? "bme280 on" : "bme280 off");
      break;
    case '2':
      enableVeml7700 = !enableVeml7700;
      Serial.println(enableVeml7700 ? "veml7700 on" : "veml7700 off");
      break;
    case '3':
      enableScd41 = !enableScd41;
      Serial.println(enableScd41 ? "scd41 on" : "scd41 off");
      break;
    case '4':
      enableMicrophone = !enableMicrophone;
      Serial.println(enableMicrophone ? "microphone on" : "microphone off");
      break;
    case '5':
      enableBattery = !enableBattery;
      Serial.println(enableBattery ? "battery on" : "battery off");
      break;
    case '+':
      if (settleMs + kSettleStepMs <= kSettleMaxMs) {
        settleMs += kSettleStepMs;
      }
      Serial.print("settle=");
      Serial.print(settleMs);
      Serial.println("ms");
      break;
    case '-':
      if (settleMs >= kSettleStepMs * 2) {
        settleMs -= kSettleStepMs;
      }
      Serial.print("settle=");
      Serial.print(settleMs);
      Serial.println("ms");
      break;
    default:
      break;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  power.beginOff();  // also parks the battery-enable pin, which BatteryMonitor
                     // drives but does not configure
  batteryMonitor.begin();
  UnitIdentity::formatSerial(DeviceId::read(), unitSerial);
  snprintf(bleName, sizeof bleName, "Quiesco TEST %s",
           unitSerial + UnitIdentity::kSerialChars -
               UnitIdentity::kNameSuffixChars);
  // Do not gate the banner on Serial: the USB CDC port re-enumerates after
  // upload, so anything printed once in setup() is usually gone before the
  // monitor attaches (HARDWARE.md §6.5). The prompt below repeats until a key
  // arrives, so a blank console can only mean the board is dead or the port
  // is wrong.
}

void loop() {
  static uint32_t lastPromptMs = 0;
  static bool greeted = false;

  while (Serial.available() > 0) {
    greeted = true;
    handleCommand(static_cast<char>(Serial.read()));
  }

  if (!greeted && millis() - lastPromptMs >= 2000) {
    lastPromptMs = millis();
    Serial.println("Quiesco unit test -- press 't' to test this unit, '?' for all commands");
  }

  serviceCycle();
  serviceBle();
  delay(1);
}
