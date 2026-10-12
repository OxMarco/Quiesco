// SPDX-License-Identifier: GPL-3.0-only
// Quiesco RF test firmware, for the CE and FCC test lab only. Never ship it.
//
// The production firmware only advertises and connects, so the radio is on
// for a few hundred microseconds at a time and on a channel the stack picks.
// Radiated tests need the opposite: one fixed channel, transmitting all the
// time. This sketch drives the nRF52840 RADIO registers directly (no BLE
// stack is linked) and offers:
//
//   1 / 2 / 3   modulated TX on 2402 / 2440 / 2480 MHz: back-to-back BLE 1M
//               packets, 255-byte PRBS9 payload, almost 100% duty cycle
//   4 / 5 / 6   unmodulated carrier on 2402 / 2440 / 2480 MHz
//   0           radio off
//   p           TX power: +8 dBm (default, the nRF52840 maximum) / 0 dBm
//   a           host activity on/off: peripheral rail up, e-ink full refresh
//               every 10 s, flash reads and I2C traffic in between -- the
//               "host operating while the module transmits" state of
//               FCC KDB 996369 D04
//   u           restart as the XIAO-BOOT USB drive, to load another .uf2
//   ?           status and help
//
// At power-up it starts in modulated TX on 2440 MHz at +8 dBm with host
// activity off, so a unit that loses power comes back in a known mode. The
// mode is chosen over USB serial (115200), then the cable can be unplugged:
// the unit keeps transmitting on its battery (most of a day at +8 dBm).
//
// +8 dBm is the hardware maximum and covers the module's RED certificate
// (8.27 dBm), so every result here is a worst case for the product.
//
// Board: XIAO nRF52840 (Sense) Plus, Seeeduino mbed 2.9.3. `src/` here is a
// tree of symlinks into the firmware's own `src/`, as in smoke/sensors.

#include <Arduino.h>
#include <Wire.h>

#if !defined(ARDUINO_SEEED_XIAO_NRF52840_PLUS) && \
    !defined(ARDUINO_SEEED_XIAO_NRF52840_SENSE_PLUS)
#error "Select board: XIAO nRF52840 (Sense) Plus - Seeed nRF52 mbed core"
#endif

#include "src/board/PowerDomain.h"
#include "src/drivers/EpaperDisplay.h"
#include "src/drivers/W25Q64Flash.h"
#include "src/platform/BootloaderEntry.h"

namespace {

enum class TxMode : uint8_t { kOff, kModulated, kCarrier };

// RF channel k is 2402 + 2k MHz (BLE numbering across the whole band, not
// the BLE logical channel index). Low, middle and high, as the lab tests them.
constexpr uint8_t kChannelLow = 0;    // 2402 MHz
constexpr uint8_t kChannelMid = 19;   // 2440 MHz
constexpr uint8_t kChannelHigh = 39;  // 2480 MHz

constexpr uint32_t kHostRefreshMs = 10000;
constexpr uint32_t kHostBusStepMs = 100;
constexpr uint32_t kStatusMs = 5000;

// BLE advertising access address, so a sniffer recognises the packets.
constexpr uint32_t kAccessAddress = 0x8E89BED6;
constexpr uint8_t kPayloadBytes = 255;

// S0, LENGTH, payload: the RADIO reads the packet from here by EasyDMA, so it
// must stay in RAM for as long as TX runs.
uint8_t packet[2 + kPayloadBytes];

TxMode mode = TxMode::kModulated;
uint8_t channel = kChannelMid;
bool fullPower = true;
bool hostActivity = false;

PowerDomain power;
EpaperDisplay display;
W25Q64Flash flash;

uint32_t nextRefreshMs = 0;
uint32_t nextBusStepMs = 0;
uint32_t nextStatusMs = 0;
bool invertPattern = false;
uint32_t refreshCount = 0;
uint32_t refreshFailures = 0;
uint32_t hfxoRestarts = 0;

// ---- Radio ----------------------------------------------------------------

void fillPrbs9(uint8_t* data, size_t length) {
  uint16_t state = 0x1FF;
  for (size_t i = 0; i < length; ++i) {
    uint8_t byte = 0;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint16_t next = ((state >> 5) ^ (state >> 4)) & 1;  // x^9 + x^5 + 1
      byte |= static_cast<uint8_t>((state & 1) << bit);
      state = static_cast<uint16_t>((state >> 1) | (next << 8));
    }
    data[i] = byte;
  }
}

// The radio needs the 32 MHz crystal: on the internal RC oscillator the
// carrier would sit several MHz off frequency. Nothing in this sketch stops
// the crystal, but the core's USB driver may release it when the cable is
// unplugged, so loop() checks it and restarts TX if it ever drops.
bool hfxoRunning() {
  const uint32_t status = NRF_CLOCK->HFCLKSTAT;
  return (status & CLOCK_HFCLKSTAT_SRC_Msk) ==
             (CLOCK_HFCLKSTAT_SRC_Xtal << CLOCK_HFCLKSTAT_SRC_Pos) &&
         (status & CLOCK_HFCLKSTAT_STATE_Msk) ==
             (CLOCK_HFCLKSTAT_STATE_Running << CLOCK_HFCLKSTAT_STATE_Pos);
}

void hfxoStart() {
  NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
  NRF_CLOCK->TASKS_HFCLKSTART = 1;
  const uint32_t startMs = millis();
  while (NRF_CLOCK->EVENTS_HFCLKSTARTED == 0 && millis() - startMs < 100) {
  }
}

void radioDisable() {
  NRF_RADIO->SHORTS = 0;
  NRF_RADIO->EVENTS_DISABLED = 0;
  NRF_RADIO->TASKS_DISABLE = 1;
  const uint32_t startMs = millis();
  while (NRF_RADIO->EVENTS_DISABLED == 0 && millis() - startMs < 10) {
  }
  NRF_RADIO->EVENTS_DISABLED = 0;
}

void radioApply() {
  radioDisable();
  if (mode == TxMode::kOff) {
    return;
  }
  if (!hfxoRunning()) {
    hfxoStart();
  }

  NRF_RADIO->MODE = RADIO_MODE_MODE_Ble_1Mbit << RADIO_MODE_MODE_Pos;
  NRF_RADIO->TXPOWER =
      (fullPower ? RADIO_TXPOWER_TXPOWER_Pos8dBm : RADIO_TXPOWER_TXPOWER_0dBm)
      << RADIO_TXPOWER_TXPOWER_Pos;
  NRF_RADIO->FREQUENCY = 2u + 2u * channel;  // MHz above 2400

  if (mode == TxMode::kCarrier) {
    // TXEN without START ramps up and holds TXIDLE, where the radio sends
    // an unmodulated carrier until it is disabled.
    NRF_RADIO->TASKS_TXEN = 1;
    return;
  }

  // BLE 1M framing: 8-bit preamble, 4-byte access address, S0 + 8-bit
  // LENGTH, 24-bit CRC. Whitening stays off: PRBS9 is already random.
  NRF_RADIO->PCNF0 = (1u << RADIO_PCNF0_S0LEN_Pos) |
                     (8u << RADIO_PCNF0_LFLEN_Pos) |
                     (RADIO_PCNF0_PLEN_8bit << RADIO_PCNF0_PLEN_Pos);
  NRF_RADIO->PCNF1 = (kPayloadBytes << RADIO_PCNF1_MAXLEN_Pos) |
                     (3u << RADIO_PCNF1_BALEN_Pos) |
                     (RADIO_PCNF1_ENDIAN_Little << RADIO_PCNF1_ENDIAN_Pos);
  NRF_RADIO->BASE0 = kAccessAddress << 8;
  NRF_RADIO->PREFIX0 = kAccessAddress >> 24;
  NRF_RADIO->TXADDRESS = 0;
  NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Three << RADIO_CRCCNF_LEN_Pos) |
                      (RADIO_CRCCNF_SKIPADDR_Skip << RADIO_CRCCNF_SKIPADDR_Pos);
  NRF_RADIO->CRCPOLY = 0x00065B;
  NRF_RADIO->CRCINIT = 0x555555;
  NRF_RADIO->PACKETPTR = reinterpret_cast<uint32_t>(packet);

  // READY->START and END->START chain the packets in hardware, so TX keeps
  // going while the CPU is busy with the host activity or a panel refresh.
  NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_START_Msk;
  NRF_RADIO->TASKS_TXEN = 1;
}

// ---- Host activity ----------------------------------------------------------

// Checkerboard, inverted on every refresh so each one drives every pixel and
// runs the gate-voltage boost for the full waveform.
void drawPattern() {
  using Ink = EpaperDisplay::Ink;
  constexpr int16_t kCell = EpaperDisplay::kWidth / 8;
  display.beginFrame();
  for (int16_t row = 0; row < 8; ++row) {
    for (int16_t col = 0; col < 8; ++col) {
      if (((row + col) & 1) != invertPattern) {
        display.fillRect(col * kCell, row * kCell, kCell, kCell, Ink::kBlack);
      }
    }
  }
}

void refreshDisplay() {
  flash.end();  // the panel and the flash share the SPI bus
  bool ok = display.begin(true);
  if (ok) {
    drawPattern();
    ok = display.endFrame();
  }
  ok = display.end() && ok;
  invertPattern = !invertPattern;
  refreshCount++;
  if (!ok) {
    refreshFailures++;
  }
  flash.begin();
}

// One step of bus traffic between refreshes: a 4 KB flash read on SPI and an
// address probe of each sensor on I2C, so both bus clocks keep running.
void busStep() {
  static uint8_t buffer[W25Q64Flash::kSectorBytes];
  static uint32_t address = 0;
  flash.read(address, buffer, sizeof buffer);
  address = (address + sizeof buffer) % W25Q64Flash::kCapacityBytes;

  const uint8_t addresses[] = {0x76, 0x77, 0x10, 0x62};
  for (uint8_t sensor : addresses) {
    Wire.beginTransmission(sensor);
    Wire.endTransmission();
  }
}

void hostStart() {
  power.enable(millis());
  delay(PowerDomain::kSettleMs);
  Wire.begin();
  Wire.setClock(100000);
  flash.begin();
  nextRefreshMs = millis();
  nextBusStepMs = millis();
}

void hostStop() {
  flash.end();
  Wire.end();
  power.disable();
}

void serviceHost() {
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - nextRefreshMs) >= 0) {
    refreshDisplay();
    nextRefreshMs = millis() + kHostRefreshMs;
  } else if (static_cast<int32_t>(now - nextBusStepMs) >= 0) {
    busStep();
    nextBusStepMs = now + kHostBusStepMs;
  }
}

// ---- Console ------------------------------------------------------------------

void printStatus() {
  Serial.print("radio: ");
  switch (mode) {
    case TxMode::kOff:
      Serial.print("off");
      break;
    case TxMode::kModulated:
      Serial.print("modulated TX");
      break;
    case TxMode::kCarrier:
      Serial.print("unmodulated carrier");
      break;
  }
  if (mode != TxMode::kOff) {
    Serial.print(" at ");
    Serial.print(2402 + 2 * channel);
    Serial.print(" MHz, ");
    Serial.print(fullPower ? "+8" : "0");
    Serial.print(" dBm");
  }
  Serial.print(" | host activity: ");
  Serial.print(hostActivity ? "on" : "off");
  if (hostActivity) {
    Serial.print(" (refreshes ");
    Serial.print(refreshCount);
    Serial.print(", failed ");
    Serial.print(refreshFailures);
    Serial.print(")");
  }
  Serial.print(" | 32 MHz crystal: ");
  Serial.print(hfxoRunning() ? "running" : "STOPPED");
  if (hfxoRestarts > 0) {
    Serial.print(" (restarted ");
    Serial.print(hfxoRestarts);
    Serial.print("x)");
  }
  Serial.println();
}

void printHelp() {
  Serial.println("Quiesco RF test firmware -- for the test lab only");
  Serial.println("  1 / 2 / 3  modulated TX at 2402 / 2440 / 2480 MHz");
  Serial.println("  4 / 5 / 6  unmodulated carrier at 2402 / 2440 / 2480 MHz");
  Serial.println("  0          radio off");
  Serial.println("  p          TX power +8 dBm / 0 dBm");
  Serial.println("  a          host activity on / off (display, flash, I2C)");
  Serial.println("  u          restart as the XIAO-BOOT drive (load another .uf2)");
  Serial.println("  ?          this help and the status");
  printStatus();
}

void setTx(TxMode newMode, uint8_t newChannel) {
  mode = newMode;
  channel = newChannel;
  radioApply();
  printStatus();
}

void handleCommand(char command) {
  switch (command) {
    case '1': setTx(TxMode::kModulated, kChannelLow); break;
    case '2': setTx(TxMode::kModulated, kChannelMid); break;
    case '3': setTx(TxMode::kModulated, kChannelHigh); break;
    case '4': setTx(TxMode::kCarrier, kChannelLow); break;
    case '5': setTx(TxMode::kCarrier, kChannelMid); break;
    case '6': setTx(TxMode::kCarrier, kChannelHigh); break;
    case '0': setTx(TxMode::kOff, channel); break;
    case 'p':
      fullPower = !fullPower;
      radioApply();
      printStatus();
      break;
    case 'a':
      hostActivity = !hostActivity;
      if (hostActivity) {
        hostStart();
      } else {
        hostStop();
      }
      printStatus();
      break;
    case 'u':
      Serial.println("restarting as XIAO-BOOT...");
      Serial.flush();
      radioDisable();
      if (hostActivity) {
        hostStop();
      }
      BootloaderEntry::enterUsbUpdate();
      break;
    case '?':
      printHelp();
      break;
    default:
      break;
  }
}

}  // namespace

void setup() {
  power.beginOff();
  packet[0] = 0;  // S0: PDU header byte, contents irrelevant for the test
  packet[1] = kPayloadBytes;
  fillPrbs9(packet + 2, kPayloadBytes);
  hfxoStart();
  radioApply();

  Serial.begin(115200);
}

void loop() {
  while (Serial.available() > 0) {
    handleCommand(static_cast<char>(Serial.read()));
  }

  if (mode != TxMode::kOff && !hfxoRunning()) {
    hfxoRestarts++;
    radioApply();
  }

  if (hostActivity) {
    serviceHost();
  }

  // The port opens long after setup() on a cold plug, so the status repeats.
  const uint32_t now = millis();
  if (Serial && static_cast<int32_t>(now - nextStatusMs) >= 0) {
    printStatus();
    nextStatusMs = now + kStatusMs;
  }
  delay(1);
}
