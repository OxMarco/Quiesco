// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

#include "../protocol/LittleEndian.h"

// The user's sleep window, pushed by the app (PROTOCOL.md §6.17) so the panel
// can judge the room the way the app does: on the sleep bands from an hour
// before bedtime until wake, and only CO2 and noise by day. Kept apart from
// Config, in its own flash key, so adding it did not bump
// Config::kCurrentVersion (which resets every setting).
//
// Times are minutes after local midnight; local time is the synced UTC clock
// plus utcOffsetMinutes, which the app rewrites on every connect so daylight
// saving changes follow.
struct SleepWindow {
  static constexpr uint16_t kBytes = 12;
  static constexpr uint8_t kFlagFollowApp = 1u << 0;
  static constexpr uint16_t kNoWeekend = 0xFFFF;
  static constexpr uint16_t kMinutesPerDay = 24 * 60;
  static constexpr int16_t kMinOffsetMinutes = -720;
  static constexpr int16_t kMaxOffsetMinutes = 840;

  uint8_t flags;
  int16_t utcOffsetMinutes;
  uint16_t weekdayBedMin;
  uint16_t weekdayWakeMin;
  uint16_t weekendBedMin;   // kNoWeekend: weekend nights use the weekday times
  uint16_t weekendWakeMin;  // kNoWeekend together with weekendBedMin

  bool followsApp() const { return (flags & kFlagFollowApp) != 0; }
  bool hasWeekend() const { return weekendBedMin != kNoWeekend; }
};

// A fresh or factory-reset unit: the app's default guess (23:30-07:00), not
// followed until the app turns it on.
inline SleepWindow defaultSleepWindow() {
  SleepWindow window = {};
  window.flags = 0;
  window.utcOffsetMinutes = 0;
  window.weekdayBedMin = 23 * 60 + 30;
  window.weekdayWakeMin = 7 * 60;
  window.weekendBedMin = SleepWindow::kNoWeekend;
  window.weekendWakeMin = SleepWindow::kNoWeekend;
  return window;
}

inline bool isValidSleepWindow(const SleepWindow& window) {
  const bool noWeekend = window.weekendBedMin == SleepWindow::kNoWeekend &&
                         window.weekendWakeMin == SleepWindow::kNoWeekend;
  const bool weekendValid =
      window.weekendBedMin < SleepWindow::kMinutesPerDay &&
      window.weekendWakeMin < SleepWindow::kMinutesPerDay;
  return (window.flags & ~SleepWindow::kFlagFollowApp) == 0 &&
         window.utcOffsetMinutes >= SleepWindow::kMinOffsetMinutes &&
         window.utcOffsetMinutes <= SleepWindow::kMaxOffsetMinutes &&
         window.weekdayBedMin < SleepWindow::kMinutesPerDay &&
         window.weekdayWakeMin < SleepWindow::kMinutesPerDay &&
         (noWeekend || weekendValid);
}

// The 12-byte layout, shared by the BLE characteristic and the flash record.
inline void encodeSleepWindow(const SleepWindow& window,
                              uint8_t out[SleepWindow::kBytes]) {
  out[0] = window.flags;
  out[1] = 0;
  LittleEndian::putI16(out + 2, window.utcOffsetMinutes);
  LittleEndian::putU16(out + 4, window.weekdayBedMin);
  LittleEndian::putU16(out + 6, window.weekdayWakeMin);
  LittleEndian::putU16(out + 8, window.weekendBedMin);
  LittleEndian::putU16(out + 10, window.weekendWakeMin);
}

// False (window untouched) on a wrong length, a reserved byte or bit, or any
// field out of range.
inline bool decodeSleepWindow(const uint8_t* data, uint16_t length,
                              SleepWindow& window) {
  if (length != SleepWindow::kBytes || data[1] != 0) {
    return false;
  }
  SleepWindow candidate;
  candidate.flags = data[0];
  candidate.utcOffsetMinutes = LittleEndian::getI16(data + 2);
  candidate.weekdayBedMin = LittleEndian::getU16(data + 4);
  candidate.weekdayWakeMin = LittleEndian::getU16(data + 6);
  candidate.weekendBedMin = LittleEndian::getU16(data + 8);
  candidate.weekendWakeMin = LittleEndian::getU16(data + 10);
  if (!isValidSleepWindow(candidate)) {
    return false;
  }
  window = candidate;
  return true;
}
