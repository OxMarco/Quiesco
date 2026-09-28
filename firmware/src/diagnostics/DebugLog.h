// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

struct Config;
struct FaultStatus;
struct Reading;
struct SampleRecord;
enum class ScreenId : uint8_t;

namespace DebugLog {

// Opens USB serial and prints the boot line: firmware version, serial and
// what caused the reset.
void begin(const char* firmwareVersion, const char* serialNumber,
           const char* resetReason);
void cycleComplete(unsigned long cycleNumber, const Reading& reading,
                   const FaultStatus& faults, ScreenId screen, bool charging,
                   bool rendered, uint32_t logSequence);

// Debug console: one command character from USB serial, or 0 (always 0 in a
// release build, which never opens the port). See SOFTWARE.md §2.
char pollCommand();
void help();

struct Info {
  const char* firmwareVersion;
  const char* serialNumber;
  const char* resetReason;
  uint32_t bootCount;
  uint32_t lastSequence;
  uint8_t bondCount;
  bool usbPowered;
  uint64_t uptimeMs;
};
void info(const Info& info, const Config& config);

// Log dump as CSV between "# begin" and "# end" marker lines, which
// scripts/quiesco-console.py waits for.
void logDumpBegin(const char* serialNumber, uint32_t lastSequence);
void logDumpRecord(const SampleRecord& record);
void logDumpEnd(uint32_t records, bool complete);

// One timestamped event line, "t=<ms since boot> <what> <value>", for tracing
// short sequences such as BLE pairing. No-op in a release build.
void event(const char* what, long value = 0);

// Power-pull stress progress ('w').
void stress(uint32_t operations, uint32_t failures, uint32_t lastSequence,
            bool done);

}  // namespace DebugLog
