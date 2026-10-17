// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>

// Why the previous boot ended, worked out at the next boot from the reset
// cause and what that boot left in retained RAM (platform/RetainedRam).
// RAM survives a watchdog, lockup, soft or pin reset on the nRF52840, not a
// power loss, so after a power cut there is nothing to read and nothing is
// recorded. Pure logic, so the host tests cover it.
namespace ResetHistory {

// Refreshed by App::step() at every watchdog feed.
struct Breadcrumb {
  uint32_t bootCount = 0;     // 0 until the boot counter was read from flash
  uint32_t feeds = 0;         // watchdog feeds this boot
  uint64_t feedUptimeMs = 0;  // monotonic time of the last feed
  uint64_t feedEpochMs = 0;   // wall clock then; 0 when not synced
  uint8_t state = 0;          // App::State then
  uint16_t heapKiB = 0;       // heap in use at the last cycle start
};

// Written by the Mbed OS error hook just before a fatal error halts the CPU
// (and the watchdog then resets it).
struct FaultCapture {
  uint32_t status = 0;   // mbed_error_status_t
  uint32_t address = 0;  // PC at a CPU fault, else the caller of mbed_error
  uint32_t feeds = 0;    // Breadcrumb::feeds when it was raised
};

// The last unexpected reset: persisted in the config store and published on
// 0013 (PROTOCOL.md §6.18) in this layout.
struct Record {
  uint32_t bootCount = 0;  // the boot that ended; 0 = unknown or none
  uint32_t uptimeS = 0;    // its uptime at the last watchdog feed
  uint8_t cause = 0;       // compactCause() of the reset
  uint8_t state = 0;
  uint16_t heapKiB = 0;
  uint32_t errorStatus = 0;  // 0 = no fatal error raised
  uint32_t errorAddress = 0;
};

constexpr size_t kRecordBytes = 20;

// Compact cause bits: RESETREAS bits 0-3 (pin, watchdog, soft, lockup).
constexpr uint8_t kCausePin = 1u << 0;
constexpr uint8_t kCauseWatchdog = 1u << 1;
constexpr uint8_t kCauseSoft = 1u << 2;
constexpr uint8_t kCauseLockup = 1u << 3;

uint8_t compactCause(uint32_t resetBits);

// A watchdog or CPU lockup reset: the firmware stopped. Pin and soft resets
// are people and uploads (including the 1200-baud touch, which the firmware
// never sees), and a power-on reset leaves no RAM to read.
bool unexpected(uint32_t resetBits);

// Fills `record` for an unexpected reset; false otherwise. `previous` and
// `fault` are null when retained RAM held nothing valid.
bool describe(uint32_t resetBits, const Breadcrumb* previous,
              const FaultCapture* fault, Record& record);

// The wall clock at the moment of the reset, when it can be bounded: the
// previous boot had the time, and the reset followed its last feed by a known
// delay (the watchdog timeout, or at once for a lockup). Pin and soft resets
// can hide minutes in the bootloader, so they never qualify.
bool epochAtReset(uint32_t resetBits, const Breadcrumb* previous,
                  uint32_t watchdogTimeoutMs, uint64_t& epochMs);

void encode(const Record& record, uint8_t out[kRecordBytes]);
bool decode(const uint8_t* data, size_t length, Record& record);

}  // namespace ResetHistory
