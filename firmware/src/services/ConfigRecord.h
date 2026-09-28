// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

struct Config;

// On-flash configuration record codec. Pure C++ (host-tested); the
// flash-facing side lives in ConfigStore.
//
// Fixed 64-byte record: magic, write sequence, serialized Config v2 and a
// trailing CRC-16/CCITT. Fits one flash page, so a record write is a single
// program operation. FlashDB's KVDB provides the power-loss safety.
namespace ConfigRecordLayout {

constexpr uint32_t kRecordBytes = 64;
constexpr uint32_t kMagic = 0x47464351;  // "QCFG" read little-endian

}  // namespace ConfigRecordLayout

void encodeConfigRecord(const Config& config, uint32_t sequence,
                        uint8_t out[ConfigRecordLayout::kRecordBytes]);

// False on bad magic, bad CRC, or a decoded config that fails isValidConfig
// (which also rejects any other on-flash version).
bool decodeConfigRecord(const uint8_t in[ConfigRecordLayout::kRecordBytes],
                        Config& config, uint32_t& sequence);
