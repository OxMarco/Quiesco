// SPDX-License-Identifier: GPL-3.0-only
#include "BleCodec.h"

#include <string.h>

#include "../model/Config.h"
#include "../model/FaultStatus.h"
#include "../model/Reading.h"
#include "../model/SleepWindow.h"
#include "../services/SampleRecord.h"
#include "../platform/HmacSha256.h"
#include "LittleEndian.h"

namespace BleCodec {

namespace {

namespace le = LittleEndian;

static_assert(kSleepWindowBytes == SleepWindow::kBytes,
              "the sleep window layout lives in model/SleepWindow.h");

// Log packet header byte offsets (kLogHeaderBytes total).
constexpr uint32_t kOffType = 0;
constexpr uint32_t kOffPacketSeq = 1;
constexpr uint32_t kOffFirstSequence = 2;
constexpr uint32_t kOffRecordCount = 6;
constexpr uint32_t kOffFragIndex = 7;
constexpr uint32_t kOffRemaining = 8;
constexpr uint32_t kOffCrc = 11;

int64_t scaledClamped(float value, float scale, int64_t lo, int64_t hi) {
  const float scaled = value * scale;
  const int64_t rounded =
      static_cast<int64_t>(scaled + (scaled >= 0.0f ? 0.5f : -0.5f));
  if (rounded < lo) {
    return lo;
  }
  return rounded > hi ? hi : rounded;
}

void sealLogPacket(uint8_t* out, uint16_t packetLength) {
  out[kOffCrc] = 0;
  out[kOffCrc] = crc8(out, packetLength);
}

void putLogHeader(uint8_t* out, uint8_t type, uint8_t packetSeq,
                  uint32_t firstSequence, uint8_t recordCount,
                  uint8_t fragIndex, uint16_t remainingRecords) {
  out[kOffType] = type;
  out[kOffPacketSeq] = packetSeq;
  le::putU32(out + kOffFirstSequence, firstSequence);
  out[kOffRecordCount] = recordCount;
  out[kOffFragIndex] = fragIndex;
  le::putU16(out + kOffRemaining, remainingRecords);
  out[10] = 0;
}

}  // namespace

void encodeDeviceInfo(const DeviceInfo& info,
                      uint8_t out[kDeviceInfoBytes]) {
  memset(out, 0, kDeviceInfoBytes);
  le::putU16(out + 0, kProtocolVersion);
  le::putU32(out + 2, kCapabilities);
  out[6] = info.firmwareMajor;
  out[7] = info.firmwareMinor;
  out[8] = info.firmwarePatch;
  out[9] = static_cast<uint8_t>((info.debugBuild ? 1 : 0) |
                                (info.pairingOpen ? 2 : 0) |
                                (info.bonded ? 4 : 0) |
                                (info.noBattery ? 8 : 0));
  for (uint8_t i = 0; i < 6; i++) {  // 48-bit serial, little-endian
    out[10 + i] = static_cast<uint8_t>(info.scd41Serial >> (8 * i));
  }
  le::putU32(out + 16, info.bootCount);
}

void encodeDiagnostics(const Diagnostics& diagnostics,
                       uint8_t out[kDiagnosticsBytes]) {
  memset(out, 0, kDiagnosticsBytes);
  le::putU32(out + 0, diagnostics.resetReason);
  le::putU32(out + 4, diagnostics.uptimeSeconds);
  out[8] = diagnostics.i2cBusStuck ? 1 : 0;
}

void encodeCoreConfig(const Config& config, uint8_t out[kCoreConfigBytes]) {
  memset(out, 0, kCoreConfigBytes);
  le::putU32(out + 0, config.version);
  le::putU32(out + 4, config.measurementIntervalSeconds);
  le::putU32(out + 8, config.fullRefreshEveryCycles);
  out[12] = config.bleAlwaysAvailable ? 1 : 0;
  out[13] = config.displayScreen;
  out[14] = config.temperatureUnit;
}

void encodeStatus(const StatusInfo& info, uint8_t out[kStatusBytes]) {
  memset(out, 0, kStatusBytes);
  const FaultStatus& faults = *info.faults;
  const DeviceFault* devices[] = {
      &faults.bme280, &faults.veml7700,  &faults.scd41, &faults.battery,
      &faults.microphone, &faults.display, &faults.flash, &faults.ble,
  };
  le::putU16(out + 0, static_cast<uint16_t>(info.validFlags));
  for (uint8_t i = 0; i < 8; i++) {
    if (devices[i]->present) {
      out[2] |= static_cast<uint8_t>(1u << i);
    }
    if (devices[i]->timedOut) {
      out[3] |= static_cast<uint8_t>(1u << i);
    }
    out[4 + i] = devices[i]->consecutiveFailures > 255
                     ? 255
                     : static_cast<uint8_t>(devices[i]->consecutiveFailures);
  }
  le::putU32(out + 12, info.lastSequence);
  out[16] = static_cast<uint8_t>((info.timeSynced ? 1 : 0) |
                                 (info.syncActive ? 2 : 0) |
                                 (info.logErasing ? 4 : 0) |
                                 (info.charging ? 8 : 0));
  out[17] = info.frcState;
  le::putI16(out + 18, info.frcCorrectionPpm);
}

void encodeReadingCompact(const Reading& reading, uint8_t out[kReadingBytes]) {
  memset(out, 0, kReadingBytes);
  le::putI16(out + 0, static_cast<int16_t>(scaledClamped(
                          reading.temperatureC, 100.0f, INT16_MIN, INT16_MAX)));
  le::putU16(out + 2, static_cast<uint16_t>(scaledClamped(reading.humidityPct,
                                                          100.0f, 0, UINT16_MAX)));
  le::putU32(out + 4, static_cast<uint32_t>(scaledClamped(
                          reading.pressurePa, 1.0f, 0, UINT32_MAX)));
  le::putU16(out + 8, static_cast<uint16_t>(
                          scaledClamped(reading.co2Ppm, 1.0f, 0, UINT16_MAX)));
  le::putU32(out + 10, static_cast<uint32_t>(
                           scaledClamped(reading.lux, 10.0f, 0, UINT32_MAX)));
  le::putI16(out + 14, static_cast<int16_t>(scaledClamped(
                           reading.noiseDb, 10.0f, INT16_MIN, INT16_MAX)));
  le::putU16(out + 16, static_cast<uint16_t>(scaledClamped(
                           reading.batteryV, 1000.0f, 0, UINT16_MAX)));
  out[18] = static_cast<uint8_t>(reading.valid);
}

void encodeCalibrationOffsets(const Config& config,
                              uint8_t out[kCalibrationOffsetsBytes]) {
  le::putF32(out + 0, config.noiseOffsetDb);
  le::putF32(out + 4, config.tempOffsetC);
  le::putF32(out + 8, config.humidityOffsetRh);
}

bool decodeCoreConfig(const uint8_t* data, uint16_t length, Config& config) {
  if (length != kCoreConfigBytes || data[12] > 1 || data[15] != 0) {
    return false;
  }
  Config candidate = config;
  candidate.version = le::getU32(data + 0);
  candidate.measurementIntervalSeconds = le::getU32(data + 4);
  candidate.fullRefreshEveryCycles = le::getU32(data + 8);
  candidate.bleAlwaysAvailable = data[12] != 0;
  candidate.displayScreen = data[13];
  candidate.temperatureUnit = data[14];
  if (!isValidConfig(candidate)) {
    return false;
  }
  config = candidate;
  return true;
}

void encodeCalibrationState(const Config& config, bool ascDisabled,
                            uint8_t out[kCalibrationStateBytes]) {
  memset(out, 0, kCalibrationStateBytes);
  le::putU32(out + 0, config.frcEpochSeconds);
  le::putU16(out + 4, config.frcTargetPpm);
  le::putI16(out + 6, config.frcCorrectionPpm);
  out[8] = ascDisabled ? 1 : 0;
}

bool decodeIntervalWrite(const uint8_t* data, uint16_t length,
                         uint32_t& seconds) {
  if (length != 4) {
    return false;
  }
  const uint32_t value = le::getU32(data);
  if (!isValidMeasurementInterval(value)) {
    return false;
  }
  seconds = value;
  return true;
}

bool decodeEpochWrite(const uint8_t* data, uint16_t length, uint64_t& epochMs) {
  if (length != 8) {
    return false;
  }
  const uint64_t value = le::getU64(data);
  if (value == 0 || value / 1000 > UINT32_MAX) {
    return false;
  }
  epochMs = value;
  return true;
}

bool decodeScreenWrite(const uint8_t* data, uint16_t length, uint8_t& screen) {
  if (length != 1 || !isValidDisplayScreen(data[0])) {
    return false;
  }
  screen = data[0];
  return true;
}

bool decodeCalibrationOffsets(const uint8_t* data, uint16_t length,
                              float& noiseOffsetDb, float& tempOffsetC,
                              float& humidityOffsetRh) {
  if (length != kCalibrationOffsetsBytes) {
    return false;
  }
  const float noise = le::getF32(data + 0);
  const float temp = le::getF32(data + 4);
  const float humidity = le::getF32(data + 8);
  if (!isValidCalibrationOffsets(noise, temp, humidity)) {
    return false;
  }
  noiseOffsetDb = noise;
  tempOffsetC = temp;
  humidityOffsetRh = humidity;
  return true;
}

bool decodeCalibrationControl(const uint8_t* data, uint16_t length,
                              uint16_t& targetPpm) {
  if (length != kCalibrationControlBytes ||
      data[0] != kCalOpForcedRecalibration) {
    return false;
  }
  const uint16_t target = le::getU16(data + 2);
  if (target < 400 || target > 2000) {  // plausible ambient CO2 only
    return false;
  }
  targetPpm = target;
  return true;
}

bool decodeDeviceName(const uint8_t* data, uint16_t length, char* out) {
  if (length == 0 || length > Config::kDeviceNameMaxLength) {
    return false;
  }
  char name[Config::kDeviceNameMaxLength + 1];
  memcpy(name, data, length);
  name[length] = '\0';
  if (!isValidDeviceName(name)) {  // also rejects embedded NULs (early end)
    return false;
  }
  if (strlen(name) != length) {
    return false;
  }
  memcpy(out, name, sizeof name);
  return true;
}

bool decodeDeviceControl(const uint8_t* data, uint16_t length,
                         DeviceControlRequest& request) {
  if (length < kDeviceControlBytes ||
      le::getU16(data + 2) != kFactoryResetConfirm) {
    return false;
  }
  if (data[0] == kControlFactoryReset && length == kDeviceControlBytes &&
      (data[1] & ~kResetEraseLog) == 0) {
    request.opcode = kControlFactoryReset;
    request.eraseLog = (data[1] & kResetEraseLog) != 0;
    request.upToSequence = 0;
    return true;
  }
  if (data[0] == kControlEraseLog && length == kEraseLogControlBytes &&
      data[1] == 0) {
    request.opcode = kControlEraseLog;
    request.eraseLog = true;
    request.upToSequence = le::getU32(data + 4);
    return true;
  }
  return false;
}

void encodeSleepWindow(const SleepWindow& window,
                       uint8_t out[kSleepWindowBytes]) {
  ::encodeSleepWindow(window, out);
}

bool decodeSleepWindow(const uint8_t* data, uint16_t length,
                       SleepWindow& window) {
  return ::decodeSleepWindow(data, length, window);
}

bool decodeLogSyncControl(const uint8_t* data, uint16_t length,
                          LogSyncRequest& request) {
  if (length != kLogSyncControlBytes) {
    return false;
  }
  const uint8_t opcode = data[0];
  if (opcode != kSyncOpStart && opcode != kSyncOpAbort) {
    return false;
  }
  request.opcode = opcode;
  request.startSequence = le::getU32(data + 2);
  request.maxRecords = le::getU16(data + 6);
  request.attPayload = le::getU16(data + 8);
  return true;
}

void encodeAuthState(const AuthState& state, uint8_t out[kAuthStateBytes]) {
  memset(out, 0, kAuthStateBytes);
  out[0] = static_cast<uint8_t>((state.authenticated ? 1 : 0) |
                                (state.enrolmentOpen ? 2 : 0) |
                                (state.enrolled ? 4 : 0));
  memcpy(out + 2, state.challenge, kAuthChallengeBytes);
}

void encodeEnrolKey(uint16_t keyId, const uint8_t* issuedKey,
                    uint8_t out[kEnrolKeyBytes]) {
  memset(out, 0, kEnrolKeyBytes);
  le::putU16(out + 0, keyId);
  if (issuedKey) memcpy(out + 4, issuedKey, kAuthKeyBytes);
}

bool setupKey(uint32_t code, uint16_t keyId, uint8_t out[kAuthKeyBytes]) {
  static const uint8_t kLabel[] = {'Q', 'S', 'E', 'T', 'U', 'P', '1'};
  uint8_t digits[6];
  for (int i = 5; i >= 0; i--) {
    digits[i] = static_cast<uint8_t>('0' + code % 10);
    code /= 10;
  }
  uint8_t id[2];
  le::putU16(id, keyId);
  uint8_t mac[HmacSha256::kMacBytes];
  const bool ok = HmacSha256::compute(digits, sizeof digits, kLabel,
                                      sizeof kLabel, id, sizeof id, nullptr, 0,
                                      mac);
  memcpy(out, mac, kAuthKeyBytes);
  return ok;
}

bool decodeAuthWrite(const uint8_t* data, uint16_t length,
                     AuthRequest& request) {
  if (length < 2 || data[1] != 0) {
    return false;
  }
  if (data[0] == kAuthOpEnrol && length == kAuthEnrolBytes) {
    request.opcode = kAuthOpEnrol;
    request.keyId = 0;
    memset(request.proof, 0, sizeof request.proof);
    return true;
  }
  if (data[0] == kAuthOpProve && length == kAuthProveBytes) {
    request.opcode = kAuthOpProve;
    request.keyId = le::getU16(data + 2);
    memcpy(request.proof, data + 4, kAuthProofBytes);
    return true;
  }
  return false;
}

bool authProof(const uint8_t key[kAuthKeyBytes],
               const uint8_t challenge[kAuthChallengeBytes], uint16_t keyId,
               uint8_t out[kAuthProofBytes]) {
  static const uint8_t kLabel[] = {'Q', 'A', 'U', 'T', 'H', '1'};
  uint8_t id[2];
  le::putU16(id, keyId);
  uint8_t mac[HmacSha256::kMacBytes];
  const bool ok = HmacSha256::compute(key, kAuthKeyBytes, kLabel, sizeof kLabel,
                                      challenge, kAuthChallengeBytes, id,
                                      sizeof id, mac);
  memcpy(out, mac, kAuthProofBytes);
  return ok;
}

uint8_t crc8(const uint8_t* data, uint16_t length) {
  uint8_t crc = 0x00;
  for (uint16_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07)
                         : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc;
}

void sampleRecordToWire(const SampleRecord& record,
                        uint8_t out[kWireRecordBytes]) {
  le::putU32(out + 0, record.sequence);
  le::putU16(out + 4, record.validFlags);
  le::putU16(out + 6, record.epochSeconds != 0 ? 1 : 0);  // bit0 timeSynced
  le::putU32(out + 8, record.epochSeconds);
  le::putU64(out + 12, record.monotonicMs);
  le::putF32(out + 20, record.temperatureC);
  le::putF32(out + 24, record.humidityPct);
  le::putF32(out + 28, record.pressurePa);
  le::putF32(out + 32, record.co2Ppm);
  le::putF32(out + 36, record.lux);
  le::putF32(out + 40, record.noiseDb);
  le::putF32(out + 44, record.batteryV);
  le::putU32(out + 48, record.bootCount);
}

uint16_t clampAttPayload(uint16_t reported) {
  if (reported < kMinAttPayload) {  // includes the "not reported" 0
    return kMinAttPayload;
  }
  return reported > kMaxLogPacketBytes ? kMaxLogPacketBytes : reported;
}

uint8_t recordsPerPacket(uint16_t attPayload) {
  if (attPayload <= kLogHeaderBytes) {
    return 0;
  }
  const uint16_t budget = attPayload - kLogHeaderBytes;
  const uint16_t count = budget / kWireRecordBytes;
  return count > 255 ? 255 : static_cast<uint8_t>(count);
}

uint8_t fragmentsPerRecord(uint16_t attPayload) {
  const uint16_t budget = attPayload > kLogHeaderBytes
                              ? attPayload - kLogHeaderBytes
                              : kMinAttPayload - kLogHeaderBytes;
  return static_cast<uint8_t>((kWireRecordBytes + budget - 1) / budget);
}

uint16_t buildLogDataPacket(uint8_t packetSeq, uint16_t remainingRecords,
                            const SampleRecord* records, uint8_t count,
                            uint8_t out[kMaxLogPacketBytes]) {
  putLogHeader(out, kLogPacketData, packetSeq, records[0].sequence, count, 0,
               remainingRecords);
  uint16_t offset = kLogHeaderBytes;
  for (uint8_t i = 0; i < count; i++) {
    sampleRecordToWire(records[i], out + offset);
    offset += kWireRecordBytes;
  }
  sealLogPacket(out, offset);
  return offset;
}

uint16_t buildLogFragmentPacket(uint8_t packetSeq, uint16_t remainingRecords,
                                const uint8_t wire[kWireRecordBytes],
                                uint32_t sequence, uint8_t fragIndex,
                                uint16_t attPayload,
                                uint8_t out[kMaxLogPacketBytes]) {
  const uint16_t budget = attPayload - kLogHeaderBytes;
  const uint16_t chunkOffset = fragIndex * budget;
  if (chunkOffset >= kWireRecordBytes) {
    return 0;
  }
  const uint16_t chunkLength = kWireRecordBytes - chunkOffset < budget
                                   ? kWireRecordBytes - chunkOffset
                                   : budget;
  putLogHeader(out, kLogPacketFragment, packetSeq, sequence, 1, fragIndex,
               remainingRecords);
  memcpy(out + kLogHeaderBytes, wire + chunkOffset, chunkLength);
  const uint16_t packetLength = kLogHeaderBytes + chunkLength;
  sealLogPacket(out, packetLength);
  return packetLength;
}

uint16_t buildLogEndPacket(uint8_t packetSeq, uint32_t nextSequence,
                           uint8_t out[kMaxLogPacketBytes]) {
  putLogHeader(out, kLogPacketEnd, packetSeq, nextSequence, 0, 0, 0);
  sealLogPacket(out, kLogHeaderBytes);
  return kLogHeaderBytes;
}

}  // namespace BleCodec
