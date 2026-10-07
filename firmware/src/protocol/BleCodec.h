// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

struct Config;
struct FaultStatus;
struct Reading;
struct SampleRecord;
struct SleepWindow;

// Wire encoding for every Quiesco BLE characteristic. Pure C++ (host-tested);
// the transport lives in drivers/BleConfig. All fields little-endian; every
// decoder length- and range-checks before anything reaches the application.
//
// Fixed-point notification payloads are 20 bytes so status and latest-reading
// notifies survive the ATT-MTU-23 floor. Log records travel as 52-byte wire
// records inside CRC-8 packets; when the negotiated payload cannot hold one
// whole record the packet degrades to fragments.
//
// PROTOCOL.md (next to this file) is the app team's reference. Its worked
// examples are asserted byte-for-byte by the host tests, so a layout change
// here fails the build until the document and kProtocolVersion move with it.
namespace BleCodec {

// Bumped on any change to a frozen layout or its meaning. The app reads it
// from the device-info characteristic before touching anything else.
constexpr uint16_t kProtocolVersion = 6;

// Optional features this firmware implements, reported in device info. Bits
// are only ever added; a set bit keeps its meaning for the protocol version.
enum Capability : uint32_t {
  kCapScreenSelect = 1u << 0,
  kCapCalibrationOffsets = 1u << 1,
  kCapForcedRecalibration = 1u << 2,
  kCapDeviceRename = 1u << 3,
  kCapLogSync = 1u << 4,
  kCapFactoryReset = 1u << 5,
  kCapLogErase = 1u << 6,
  kCapBootCounter = 1u << 7,
  kCapCalibrationState = 1u << 8,
  kCapDiagnostics = 1u << 9,
  kCapAppAuth = 1u << 10,  // app-layer authentication (0010, 0011)
  kCapChargingState = 1u << 11,  // status charging flag, device info no-battery flag
  kCapTemperatureUnit = 1u << 12,  // core config byte 14
  kCapEraseLogCommand = 1u << 13,  // device control opcode 2
  kCapSleepWindow = 1u << 14,  // 0012: panel judges by time of day
};
constexpr uint32_t kCapabilities =
    kCapScreenSelect | kCapCalibrationOffsets | kCapForcedRecalibration |
    kCapDeviceRename | kCapLogSync | kCapFactoryReset | kCapLogErase |
    kCapBootCounter | kCapCalibrationState | kCapDiagnostics | kCapAppAuth |
    kCapChargingState | kCapTemperatureUnit | kCapEraseLogCommand |
    kCapSleepWindow;

constexpr uint16_t kDeviceInfoBytes = 20;
constexpr uint16_t kCoreConfigBytes = 16;
constexpr uint16_t kStatusBytes = 20;
constexpr uint16_t kReadingBytes = 20;
constexpr uint16_t kCalibrationOffsetsBytes = 12;
constexpr uint16_t kCalibrationStateBytes = 12;
constexpr uint16_t kDiagnosticsBytes = 12;
constexpr uint16_t kCalibrationControlBytes = 4;
constexpr uint16_t kDeviceControlBytes = 4;  // factory reset
constexpr uint16_t kEraseLogControlBytes = 8;
constexpr uint16_t kMaxDeviceControlBytes = kEraseLogControlBytes;
constexpr uint16_t kSleepWindowBytes = 12;
constexpr uint16_t kLogSyncControlBytes = 10;
constexpr uint16_t kAuthStateBytes = 20;
constexpr uint16_t kEnrolKeyBytes = 20;
constexpr uint16_t kAuthEnrolBytes = 2;
constexpr uint16_t kAuthProveBytes = 20;
constexpr uint16_t kAuthKeyBytes = 16;
constexpr uint16_t kAuthChallengeBytes = 16;
constexpr uint16_t kAuthProofBytes = 16;
// Setup codes are six decimal digits, 000000-999999.
constexpr uint32_t kSetupCodeCount = 1000000;
constexpr uint16_t kLogHeaderBytes = 12;
constexpr uint16_t kWireRecordBytes = 52;
constexpr uint16_t kMaxLogPacketBytes = 180;
// ATT payload floor (MTU 23 => 20 notify bytes); also the fallback when the
// app does not report its negotiated MTU in the sync request.
constexpr uint16_t kMinAttPayload = 20;

enum LogPacketType : uint8_t {
  kLogPacketData = 1,
  kLogPacketEnd = 2,
  kLogPacketFragment = 3,
};

enum SyncOpcode : uint8_t { kSyncOpStart = 1, kSyncOpAbort = 2 };
enum CalibrationOpcode : uint8_t { kCalOpForcedRecalibration = 1 };
enum DeviceControlOpcode : uint8_t {
  kControlFactoryReset = 1,
  kControlEraseLog = 2,
};
enum FactoryResetFlags : uint8_t { kResetEraseLog = 1u << 0 };
enum AuthOpcode : uint8_t { kAuthOpEnrol = 1, kAuthOpProve = 2 };
// A factory reset or log erase must also carry this value, so a stray or
// fuzzed write to the control characteristic cannot wipe a unit.
constexpr uint16_t kFactoryResetConfirm = 0xFAC7;

// Mirrored in the status payload; App owns the progression.
enum FrcState : uint8_t {
  kFrcIdle = 0,
  kFrcPending = 1,
  kFrcSoaking = 2,
  kFrcDone = 3,
  kFrcFailed = 4,
};

struct StatusInfo {
  const FaultStatus* faults;  // per-device masks/counters, member order
  uint32_t validFlags;        // ReadingValid bits of the latest reading
  uint32_t lastSequence;      // newest log record; app syncs from its cursor
  bool timeSynced;
  bool syncActive;
  bool logErasing;  // factory reset erasing the sample log
  bool charging;    // charger reports charging (debounced, as drawn)
  uint8_t frcState;  // FrcState
  int16_t frcCorrectionPpm;
};

struct DeviceInfo {
  uint8_t firmwareMajor;
  uint8_t firmwareMinor;
  uint8_t firmwarePatch;
  bool debugBuild;
  bool pairingOpen;  // on USB power: a new phone may pair now
  bool bonded;       // at least one phone is bonded
  bool noBattery;    // bench build without a cell (build.sh --no-battery)
  uint64_t scd41Serial;  // 48-bit; 0 until the sensor has been read
  uint32_t bootCount;    // this boot; 0 until read from flash
};

// App-layer authentication state for the auth characteristic (§6.15).
struct AuthState {
  bool authenticated;  // this connection proved a key
  bool enrolmentOpen;  // on USB power: a new phone may enrol now
  bool enrolled;       // at least one phone holds a key
  const uint8_t* challenge;  // kAuthChallengeBytes, fresh per attempt
};

struct AuthRequest {
  uint8_t opcode;  // AuthOpcode
  uint16_t keyId;  // prove only
  uint8_t proof[kAuthProofBytes];  // prove only
};

struct Diagnostics {
  uint32_t resetReason;    // ResetReason bits of the last reset
  uint32_t uptimeSeconds;  // since this boot
  bool i2cBusStuck;        // SDA still low after recovery, last cycle
};

// A device control write (§6.11).
struct DeviceControlRequest {
  uint8_t opcode;          // DeviceControlOpcode
  bool eraseLog;           // factory reset: also erase the sample log
  uint32_t upToSequence;   // log erase: refuse if the newest record is later
};

struct LogSyncRequest {
  uint8_t opcode;
  uint32_t startSequence;
  uint16_t maxRecords;  // 0 = no cap
  uint16_t attPayload;  // app-reported MTU-3; 0 = assume the floor
};

void encodeDeviceInfo(const DeviceInfo& info,
                      uint8_t out[kDeviceInfoBytes]);
void encodeDiagnostics(const Diagnostics& diagnostics,
                       uint8_t out[kDiagnosticsBytes]);
void encodeCoreConfig(const Config& config, uint8_t out[kCoreConfigBytes]);
void encodeStatus(const StatusInfo& info, uint8_t out[kStatusBytes]);
void encodeReadingCompact(const Reading& reading, uint8_t out[kReadingBytes]);
void encodeCalibrationOffsets(const Config& config,
                              uint8_t out[kCalibrationOffsetsBytes]);
// The recorded FRC history from config, plus whether this boot confirmed the
// SCD41's automatic self-calibration is off.
void encodeCalibrationState(const Config& config, bool ascDisabled,
                            uint8_t out[kCalibrationStateBytes]);

// Replaces the core fields of config (the rest are kept); false leaves it
// untouched. The version field must echo the value last read.
bool decodeCoreConfig(const uint8_t* data, uint16_t length, Config& config);
bool decodeIntervalWrite(const uint8_t* data, uint16_t length,
                         uint32_t& seconds);
bool decodeEpochWrite(const uint8_t* data, uint16_t length, uint64_t& epochMs);
bool decodeScreenWrite(const uint8_t* data, uint16_t length, uint8_t& screen);
bool decodeCalibrationOffsets(const uint8_t* data, uint16_t length,
                              float& noiseOffsetDb, float& tempOffsetC,
                              float& humidityOffsetRh);
bool decodeCalibrationControl(const uint8_t* data, uint16_t length,
                              uint16_t& targetPpm);
// Validates and NUL-terminates into out (17 bytes).
bool decodeDeviceName(const uint8_t* data, uint16_t length, char* out);
// Factory reset (4 bytes) or log erase (8 bytes); any other length,
// opcode, flag or confirmation is refused.
bool decodeDeviceControl(const uint8_t* data, uint16_t length,
                         DeviceControlRequest& request);
// Sleep window (§6.17), the layout in model/SleepWindow.h.
void encodeSleepWindow(const SleepWindow& window,
                       uint8_t out[kSleepWindowBytes]);
bool decodeSleepWindow(const uint8_t* data, uint16_t length,
                       SleepWindow& window);
bool decodeLogSyncControl(const uint8_t* data, uint16_t length,
                          LogSyncRequest& request);

void encodeAuthState(const AuthState& state, uint8_t out[kAuthStateBytes]);
// Enrolment metadata (§6.16): the pending key id while setup is pending, or
// the issued key id and key (issuedKey non-null) once the setup proof passed.
void encodeEnrolKey(uint16_t keyId, const uint8_t* issuedKey,
                    uint8_t out[kEnrolKeyBytes]);
// The key a phone proves during setup: the first kAuthKeyBytes of
// HMAC-SHA-256(the code as six ASCII digits, "QSETUP1" || keyId little-endian).
// False (out zeroed) when the crypto backend fails.
bool setupKey(uint32_t code, uint16_t keyId, uint8_t out[kAuthKeyBytes]);
bool decodeAuthWrite(const uint8_t* data, uint16_t length,
                     AuthRequest& request);
// First kAuthProofBytes of HMAC-SHA-256(key, "QAUTH1" || challenge || keyId
// little-endian): what a phone writes to prove it holds the key. False (out
// zeroed) when the crypto backend fails; no proof can then be checked.
bool authProof(const uint8_t key[kAuthKeyBytes],
               const uint8_t challenge[kAuthChallengeBytes], uint16_t keyId,
               uint8_t out[kAuthProofBytes]);
uint8_t crc8(const uint8_t* data, uint16_t length);  // poly 0x07, init 0x00
void sampleRecordToWire(const SampleRecord& record,
                        uint8_t out[kWireRecordBytes]);

uint16_t clampAttPayload(uint16_t reported);
// Whole wire records that fit one packet at this payload; 0 selects the
// fragment path.
uint8_t recordsPerPacket(uint16_t attPayload);
uint8_t fragmentsPerRecord(uint16_t attPayload);

// All builders return the packet length in bytes.
uint16_t buildLogDataPacket(uint8_t packetSeq, uint16_t remainingRecords,
                            const SampleRecord* records, uint8_t count,
                            uint8_t out[kMaxLogPacketBytes]);
uint16_t buildLogFragmentPacket(uint8_t packetSeq, uint16_t remainingRecords,
                                const uint8_t wire[kWireRecordBytes],
                                uint32_t sequence, uint8_t fragIndex,
                                uint16_t attPayload,
                                uint8_t out[kMaxLogPacketBytes]);
uint16_t buildLogEndPacket(uint8_t packetSeq, uint32_t nextSequence,
                           uint8_t out[kMaxLogPacketBytes]);

}  // namespace BleCodec
