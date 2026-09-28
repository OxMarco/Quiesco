// SPDX-License-Identifier: GPL-3.0-only
#include "PowerDomain.h"

#include <Arduino.h>

#include "BoardPins.h"

void PowerDomain::beginOff() {
  // Preload the output latch before changing direction so boot cannot pulse
  // the active-low rail on between pinMode() and digitalWrite().
  digitalWrite(BoardPins::kPeripheralRail, HIGH);
  pinMode(BoardPins::kPeripheralRail, OUTPUT);

  digitalWrite(BoardPins::kBatteryEnable, HIGH);
  pinMode(BoardPins::kBatteryEnable, OUTPUT);
  // Keep inactive levels in the CS output latches even while their pins are
  // parked as inputs with the peripheral rail off.
  digitalWrite(BoardPins::kFlashCs, HIGH);
  digitalWrite(BoardPins::kEpaperCs, HIGH);
  parkRailOffPins();
  enabled_ = false;
  readyAtMs_ = 0;
}

void PowerDomain::enable(uint64_t nowMs, uint32_t settleMs) {
  digitalWrite(BoardPins::kPeripheralRail, LOW);
  // Both latches were preloaded while the rail was off, so changing direction
  // cannot produce the first-boot LOW pulse that would select a bus device.
  parkPoweredPins();
  enabled_ = true;
  readyAtMs_ = nowMs + settleMs;
}

bool PowerDomain::ready(uint64_t nowMs) const {
  return enabled_ && nowMs >= readyAtMs_;
}

void PowerDomain::disable() {
  digitalWrite(BoardPins::kBatteryEnable, HIGH);
  digitalWrite(BoardPins::kFlashCs, HIGH);
  digitalWrite(BoardPins::kEpaperCs, HIGH);
  digitalWrite(BoardPins::kPeripheralRail, HIGH);
  parkRailOffPins();
  enabled_ = false;
  readyAtMs_ = 0;
}

bool PowerDomain::enabled() const {
  return enabled_;
}

void PowerDomain::parkPoweredPins() {
  digitalWrite(BoardPins::kFlashCs, HIGH);
  pinMode(BoardPins::kFlashCs, OUTPUT);
  digitalWrite(BoardPins::kEpaperCs, HIGH);
  pinMode(BoardPins::kEpaperCs, OUTPUT);
}

void PowerDomain::parkRailOffPins() {
  pinMode(BoardPins::kFlashCs, INPUT);
  pinMode(BoardPins::kEpaperCs, INPUT);
  pinMode(BoardPins::kEpaperBusy, INPUT);
  pinMode(BoardPins::kEpaperReset, INPUT);
  pinMode(BoardPins::kEpaperDc, INPUT);
}
