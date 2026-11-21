// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

struct Config;

// On-flash configuration record codec. Pure C++ (host-tested); the
// flash-facing side lives in ConfigStore.
//
// 64-byte record: magic, write sequence, serialized Config and a trailing
// CRC-16/CCITT. Fits one flash page, so a record write is a single program
// operation. FlashDB's KVDB provides the power-loss safety.
//
// Compatibility rules, so a firmware update or downgrade never costs an owner
// their settings and calibration:
//  - The byte offsets of existing fields never change.
//  - A later firmware that needs more room writes a longer record (up to
//    kMaxRecordBytes): the first kRecordBytes - 2 bytes keep this layout, new
//    fields follow, and the CRC covers everything before its own last two
//    bytes. This firmware reads the fields it knows from such a record.
namespace ConfigRecordLayout {

constexpr uint32_t kRecordBytes = 64;
constexpr uint32_t kMaxRecordBytes = 128;
constexpr uint32_t kMagic = 0x47464351;  // "QCFG" read little-endian

}  // namespace ConfigRecordLayout

enum class ConfigRecordStatus : uint8_t {
  // Not a config record, damaged, or from a development build older than
  // Config::kOldestReadableVersion: defaults may replace it.
  kInvalid,
  // Read; config.version is Config::kCurrentVersion whatever was stored.
  kLoaded,
  // An intact record from a newer firmware whose known fields this version
  // cannot use (a value it does not understand). Run on defaults, but leave
  // the record alone until the owner changes a setting.
  kNewerUnreadable,
};

void encodeConfigRecord(const Config& config, uint32_t sequence,
                        uint8_t out[ConfigRecordLayout::kRecordBytes]);

// `length` is the stored record's size, kRecordBytes..kMaxRecordBytes.
// config and sequence are written only for kLoaded; storedVersion (optional)
// whenever the record is intact.
ConfigRecordStatus decodeConfigRecord(const uint8_t* in, uint32_t length,
                                      Config& config, uint32_t& sequence,
                                      uint32_t* storedVersion = nullptr);
