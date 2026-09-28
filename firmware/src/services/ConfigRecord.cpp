// SPDX-License-Identifier: GPL-3.0-only
#include "ConfigRecord.h"

#include <string.h>

#include "../model/Config.h"
#include "../protocol/LittleEndian.h"
#include "SampleRecord.h"  // crc16Ccitt

namespace {

using namespace ConfigRecordLayout;
namespace le = LittleEndian;

// Byte offsets inside the 64-byte record.
constexpr uint32_t kOffMagic = 0;
constexpr uint32_t kOffSequence = 4;
constexpr uint32_t kOffVersion = 8;
constexpr uint32_t kOffInterval = 12;
constexpr uint32_t kOffFullRefresh = 16;
constexpr uint32_t kOffBleAlways = 20;
constexpr uint32_t kOffScreen = 21;
constexpr uint32_t kOffName = 22;  // 17 bytes, NUL-padded
// Zero-filled before the unit existed, so an older record decodes as °C
// without a version bump.
constexpr uint32_t kOffTempUnit = 39;
constexpr uint32_t kOffNoiseOffset = 40;
constexpr uint32_t kOffTempOffset = 44;
constexpr uint32_t kOffHumidityOffset = 48;
// 52..61 were zero-filled before the FRC fields existed, so an older record
// decodes as "no FRC recorded" without a version bump.
constexpr uint32_t kOffFrcEpoch = 52;
constexpr uint32_t kOffFrcTarget = 56;
constexpr uint32_t kOffFrcCorrection = 58;
constexpr uint32_t kOffCrc = kRecordBytes - 2;

}  // namespace

void encodeConfigRecord(const Config& config, uint32_t sequence,
                        uint8_t out[kRecordBytes]) {
  memset(out, 0, kRecordBytes);
  le::putU32(out + kOffMagic, kMagic);
  le::putU32(out + kOffSequence, sequence);
  le::putU32(out + kOffVersion, config.version);
  le::putU32(out + kOffInterval, config.measurementIntervalSeconds);
  le::putU32(out + kOffFullRefresh, config.fullRefreshEveryCycles);
  out[kOffBleAlways] = config.bleAlwaysAvailable ? 1 : 0;
  out[kOffScreen] = config.displayScreen;
  memcpy(out + kOffName, config.deviceName, sizeof config.deviceName);
  out[kOffTempUnit] = config.temperatureUnit;
  le::putF32(out + kOffNoiseOffset, config.noiseOffsetDb);
  le::putF32(out + kOffTempOffset, config.tempOffsetC);
  le::putF32(out + kOffHumidityOffset, config.humidityOffsetRh);
  le::putU32(out + kOffFrcEpoch, config.frcEpochSeconds);
  le::putU16(out + kOffFrcTarget, config.frcTargetPpm);
  le::putI16(out + kOffFrcCorrection, config.frcCorrectionPpm);
  le::putU16(out + kOffCrc, crc16Ccitt(out, kOffCrc));
}

bool decodeConfigRecord(const uint8_t in[kRecordBytes], Config& config,
                        uint32_t& sequence) {
  if (le::getU32(in + kOffMagic) != kMagic ||
      le::getU16(in + kOffCrc) != crc16Ccitt(in, kOffCrc) ||
      in[kOffBleAlways] > 1 ||
      in[kOffName + Config::kDeviceNameMaxLength] != '\0') {
    return false;
  }
  Config decoded = {};
  decoded.version = le::getU32(in + kOffVersion);
  decoded.measurementIntervalSeconds = le::getU32(in + kOffInterval);
  decoded.fullRefreshEveryCycles = le::getU32(in + kOffFullRefresh);
  decoded.bleAlwaysAvailable = in[kOffBleAlways] != 0;
  decoded.displayScreen = in[kOffScreen];
  memcpy(decoded.deviceName, in + kOffName, sizeof decoded.deviceName);
  decoded.deviceName[sizeof decoded.deviceName - 1] = '\0';
  decoded.temperatureUnit = in[kOffTempUnit];
  decoded.noiseOffsetDb = le::getF32(in + kOffNoiseOffset);
  decoded.tempOffsetC = le::getF32(in + kOffTempOffset);
  decoded.humidityOffsetRh = le::getF32(in + kOffHumidityOffset);
  decoded.frcEpochSeconds = le::getU32(in + kOffFrcEpoch);
  decoded.frcTargetPpm = le::getU16(in + kOffFrcTarget);
  decoded.frcCorrectionPpm = le::getI16(in + kOffFrcCorrection);
  if (!isValidConfig(decoded)) {
    return false;
  }
  const uint32_t decodedSequence = le::getU32(in + kOffSequence);
  if (decodedSequence == 0 || decodedSequence == UINT32_MAX) {
    return false;
  }
  config = decoded;
  sequence = decodedSequence;
  return true;
}
