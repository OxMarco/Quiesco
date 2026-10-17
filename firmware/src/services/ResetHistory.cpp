// SPDX-License-Identifier: GPL-3.0-only
#include "ResetHistory.h"

#include "../protocol/LittleEndian.h"

namespace ResetHistory {

namespace {

// RESETREAS: 0 pin, 1 watchdog, 2 soft, 3 lockup.
constexpr uint32_t kResetPin = 1u << 0;
constexpr uint32_t kResetWatchdog = 1u << 1;
constexpr uint32_t kResetSoft = 1u << 2;
constexpr uint32_t kResetLockup = 1u << 3;

namespace le = LittleEndian;

}  // namespace

uint8_t compactCause(uint32_t resetBits) {
  return static_cast<uint8_t>(resetBits & 0x0Fu);
}

bool unexpected(uint32_t resetBits) {
  return (resetBits & (kResetWatchdog | kResetLockup)) != 0;
}

bool describe(uint32_t resetBits, const Breadcrumb* previous,
              const FaultCapture* fault, Record& record) {
  if (!unexpected(resetBits)) {
    return false;
  }
  record = Record();
  record.cause = compactCause(resetBits);
  if (previous != nullptr) {
    record.bootCount = previous->bootCount;
    record.uptimeS = static_cast<uint32_t>(previous->feedUptimeMs / 1000);
    record.state = previous->state;
    record.heapKiB = previous->heapKiB;
  }
  // An error raised before the last feed was survived (a warning, say); only
  // one after it can be what stopped the firmware.
  if (fault != nullptr &&
      (previous == nullptr || fault->feeds == previous->feeds)) {
    record.errorStatus = fault->status;
    record.errorAddress = fault->address;
  }
  return true;
}

bool epochAtReset(uint32_t resetBits, const Breadcrumb* previous,
                  uint32_t watchdogTimeoutMs, uint64_t& epochMs) {
  if (previous == nullptr || previous->feedEpochMs == 0 ||
      (resetBits & (kResetPin | kResetSoft)) != 0) {
    return false;
  }
  if (resetBits & kResetWatchdog) {
    // The watchdog fires exactly one timeout after its last reload.
    epochMs = previous->feedEpochMs + watchdogTimeoutMs;
    return true;
  }
  if (resetBits & kResetLockup) {
    epochMs = previous->feedEpochMs;  // within one step of the last feed
    return true;
  }
  return false;
}

void encode(const Record& record, uint8_t out[kRecordBytes]) {
  le::putU32(out + 0, record.bootCount);
  le::putU32(out + 4, record.uptimeS);
  out[8] = record.cause;
  out[9] = record.state;
  le::putU16(out + 10, record.heapKiB);
  le::putU32(out + 12, record.errorStatus);
  le::putU32(out + 16, record.errorAddress);
}

bool decode(const uint8_t* data, size_t length, Record& record) {
  if (data == nullptr || length != kRecordBytes) {
    return false;
  }
  record.bootCount = le::getU32(data + 0);
  record.uptimeS = le::getU32(data + 4);
  record.cause = data[8];
  record.state = data[9];
  record.heapKiB = le::getU16(data + 10);
  record.errorStatus = le::getU32(data + 12);
  record.errorAddress = le::getU32(data + 16);
  return true;
}

}  // namespace ResetHistory
