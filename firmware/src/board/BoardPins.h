// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>

#if !defined(ARDUINO_SEEED_XIAO_NRF52840_PLUS) && \
    !defined(ARDUINO_SEEED_XIAO_NRF52840_SENSE_PLUS)
#error "Select board: XIAO nRF52840 (Sense) Plus - Seeed nRF52 mbed core"
#endif

namespace BoardPins {

// Arduino indices verified against the Seeeduino Mbed 2.9.3 Plus variants.
constexpr uint8_t kFlashCs = 0;       // W_NCS, P0.02
constexpr uint8_t kEpaperCs = 1;      // ED_CS, P0.03
constexpr uint8_t kEpaperBusy = 2;    // ED_BUSY, P0.28
constexpr uint8_t kEpaperReset = 3;   // ED_RES, P0.29
constexpr uint8_t kEpaperDc = 34;     // ED_DC, P0.10
constexpr uint8_t kPeripheralRail = 30;  // 3V3ON, P0.15, active low
constexpr uint8_t kBatteryEnable = 29;   // P0.14, active low
// BQ25101 ~CHG on the XIAO module itself (open drain, LOW = charging).
// VERIFY on hardware: module-level net, not on the carrier schematic.
constexpr uint8_t kChargeStatus = 22;    // ~CHG, P0.17

// Absolute nRF PSEL values, not Arduino indices.
constexpr uint32_t kPdmClockPsel = 37;  // P1.05
constexpr uint32_t kPdmDataPsel = 39;   // P1.07
// The battery ADC channel is not named here: BatteryMonitor selects it through
// the SAADC's own AnalogInput7 register field (P0.31), so a constant here would
// be a second source of truth that nothing enforces.

// Wire and SPI must use the core-provided instances. Their variant mappings
// are verified; duplicating their numeric pins here would invite misuse.
// The one exception: I2cBusRecovery clocks a stuck bus by hand before
// Wire.begin(), so it needs the raw port pins (SDA P0.04, SCL P0.05, as the
// smoke sketch's bit-bang probe uses them).
constexpr uint32_t kI2cSdaPortPin = 4;
constexpr uint32_t kI2cSclPortPin = 5;

static_assert(D0 == kFlashCs, "Unexpected D0 mapping in selected board core");
static_assert(D1 == kEpaperCs, "Unexpected D1 mapping in selected board core");
static_assert(D11 == kPeripheralRail,
              "Unexpected D11 mapping in selected board core");
static_assert(D15 == kEpaperDc,
              "Unexpected D15 mapping in selected board core");
static_assert(PIN_VBAT_ENABLE == kBatteryEnable,
              "Unexpected battery-enable mapping in selected board core");

}  // namespace BoardPins
