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
// saving changes follow. One window applies to every night; bytes 8-11 are
// reserved (always FF FF FF FF) for per-night windows later.
struct SleepWindow {
  static constexpr uint16_t kBytes = 12;
  static constexpr uint8_t kFlagFollowApp = 1u << 0;
  static constexpr uint16_t kReservedTime = 0xFFFF;
  static constexpr uint16_t kMinutesPerDay = 24 * 60;
  static constexpr int16_t kMinOffsetMinutes = -720;
  static constexpr int16_t kMaxOffsetMinutes = 840;

  uint8_t flags;
  int16_t utcOffsetMinutes;
  uint16_t bedMin;
  uint16_t wakeMin;

  bool followsApp() const { return (flags & kFlagFollowApp) != 0; }
};

// A fresh or factory-reset unit: the app's default guess (23:30-07:00), not
// followed until the app turns it on.
inline SleepWindow defaultSleepWindow() {
  SleepWindow window = {};
  window.flags = 0;
  window.utcOffsetMinutes = 0;
  window.bedMin = 23 * 60 + 30;
  window.wakeMin = 7 * 60;
  return window;
}

inline bool isValidSleepWindow(const SleepWindow& window) {
  return (window.flags & ~SleepWindow::kFlagFollowApp) == 0 &&
         window.utcOffsetMinutes >= SleepWindow::kMinOffsetMinutes &&
         window.utcOffsetMinutes <= SleepWindow::kMaxOffsetMinutes &&
         window.bedMin < SleepWindow::kMinutesPerDay &&
         window.wakeMin < SleepWindow::kMinutesPerDay &&
         // Equal times make a zero-length window: day bands all night. The
         // app never sends one (it holds sleep to a minimum length).
         window.bedMin != window.wakeMin;
}

// The 12-byte layout, shared by the BLE characteristic and the flash record.
inline void encodeSleepWindow(const SleepWindow& window,
                              uint8_t out[SleepWindow::kBytes]) {
  out[0] = window.flags;
  out[1] = 0;
  LittleEndian::putI16(out + 2, window.utcOffsetMinutes);
  LittleEndian::putU16(out + 4, window.bedMin);
  LittleEndian::putU16(out + 6, window.wakeMin);
  LittleEndian::putU16(out + 8, SleepWindow::kReservedTime);
  LittleEndian::putU16(out + 10, SleepWindow::kReservedTime);
}

// False (window untouched) on a wrong length, a reserved byte or bit (bytes
// 8-11 must be FF), or any field out of range. A flash record saved with the
// weekend times of earlier firmware fails here, so the unit falls back to the
// default window until the app writes it again on the next connect.
inline bool decodeSleepWindow(const uint8_t* data, uint16_t length,
                              SleepWindow& window) {
  if (length != SleepWindow::kBytes || data[1] != 0 ||
      LittleEndian::getU16(data + 8) != SleepWindow::kReservedTime ||
      LittleEndian::getU16(data + 10) != SleepWindow::kReservedTime) {
    return false;
  }
  SleepWindow candidate;
  candidate.flags = data[0];
  candidate.utcOffsetMinutes = LittleEndian::getI16(data + 2);
  candidate.bedMin = LittleEndian::getU16(data + 4);
  candidate.wakeMin = LittleEndian::getU16(data + 6);
  if (!isValidSleepWindow(candidate)) {
    return false;
  }
  window = candidate;
  return true;
}
