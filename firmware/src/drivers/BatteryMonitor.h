// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

// Battery voltage via direct SAADC on AIN7 (the core's Arduino analog path
// corrupts memory on this pin, see HARDWARE.md) plus charger status from the
// XIAO module's BQ25101 ~CHG pin. Both nets are on the always-on side, so
// this driver works with the peripheral rail off.
class BatteryMonitor {
 public:
  static constexpr uint32_t kChargeDebounceMs = 1000;

  void begin();

  // A burst of kBurstSamples readings kBurstSpacingMs apart, blocking for
  // ~50 ms: volts is their mean, spreadVolts (optional) the highest minus the
  // lowest. False when the SAADC never signals completion; the outputs are
  // untouched in that case.
  //
  // The spread is how a missing cell is told from a real one. With nothing
  // on the battery connector the charger's output switches between about
  // 3.75 V and 4.16 V every ~20 ms (measured on a v1 unit, 2026-11-21), so a
  // single reading looks like a healthy battery. A cell holds the rail
  // steady to within millivolts.
  bool sample(float& volts, float* spreadVolts = nullptr);
  static constexpr uint8_t kBurstSamples = 12;
  static constexpr uint32_t kBurstSpacingMs = 4;
  static constexpr float kAbsentCellSpreadVolts = 0.15f;
  static bool cellAbsent(float spreadVolts) {
    return spreadVolts > kAbsentCellSpreadVolts;
  }

  // Debounced charger state; call freely (idle polls every step). Returns the
  // stable value, flipping only after kChargeDebounceMs of agreement so a
  // blinking/floating ~CHG line cannot flap the UI.
  bool pollCharging(uint64_t nowMs);

  // USB VBUS present (nRF52840 POWER.USBREGSTATUS), independent of whether
  // the charger is still charging. Physical-presence signal for BLE pairing.
  bool usbPowered() const;

 private:
  bool chargingStable_ = false;
  bool lastRaw_ = false;
  uint64_t rawSinceMs_ = 0;
  bool primed_ = false;
};
