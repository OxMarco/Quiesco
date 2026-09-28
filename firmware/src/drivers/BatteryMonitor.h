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

  // Blocking for ~2 ms (divider settle). False when the SAADC never signals
  // completion; volts is untouched in that case.
  bool sample(float& volts);

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
