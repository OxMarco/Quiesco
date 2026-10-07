// SPDX-License-Identifier: GPL-3.0-only
#include "BleConfig.h"

#include <Arduino.h>
#include <ArduinoBLE.h>
#include <utility/HCI.h>
#include <string.h>

#include "../diagnostics/BuildConfig.h"
#include "../diagnostics/DebugLog.h"
#include "../diagnostics/FirmwareVersion.h"
#include "../model/Reading.h"
#include "../protocol/BleCodec.h"
#include "../protocol/LittleEndian.h"
#include "../platform/HmacSha256.h"
#include "../services/BondTable.h"
#include "../services/UnitIdentity.h"

namespace {

constexpr char kServiceUuid[] = "7A1E0000-8E6F-4A7A-AE32-515549455343";
constexpr char kIntervalUuid[] = "7A1E0001-8E6F-4A7A-AE32-515549455343";
constexpr char kConfigUuid[] = "7A1E0002-8E6F-4A7A-AE32-515549455343";
constexpr char kReadingUuid[] = "7A1E0003-8E6F-4A7A-AE32-515549455343";
constexpr char kStatusUuid[] = "7A1E0004-8E6F-4A7A-AE32-515549455343";
constexpr char kEpochUuid[] = "7A1E0005-8E6F-4A7A-AE32-515549455343";
constexpr char kScreenUuid[] = "7A1E0006-8E6F-4A7A-AE32-515549455343";
constexpr char kCalibrationUuid[] = "7A1E0007-8E6F-4A7A-AE32-515549455343";
constexpr char kNameUuid[] = "7A1E0008-8E6F-4A7A-AE32-515549455343";
constexpr char kSyncControlUuid[] = "7A1E0009-8E6F-4A7A-AE32-515549455343";
constexpr char kLogDataUuid[] = "7A1E000A-8E6F-4A7A-AE32-515549455343";
constexpr char kCalControlUuid[] = "7A1E000B-8E6F-4A7A-AE32-515549455343";
constexpr char kDeviceInfoUuid[] = "7A1E000C-8E6F-4A7A-AE32-515549455343";
constexpr char kDeviceControlUuid[] = "7A1E000D-8E6F-4A7A-AE32-515549455343";
constexpr char kCalibrationStateUuid[] =
    "7A1E000E-8E6F-4A7A-AE32-515549455343";
constexpr char kDiagnosticsUuid[] = "7A1E000F-8E6F-4A7A-AE32-515549455343";
constexpr char kAuthUuid[] = "7A1E0010-8E6F-4A7A-AE32-515549455343";
constexpr char kEnrolKeyUuid[] = "7A1E0011-8E6F-4A7A-AE32-515549455343";
constexpr char kSleepWindowUuid[] = "7A1E0012-8E6F-4A7A-AE32-515549455343";

// Bluetooth SIG Device Information Service and its string characteristics.
constexpr char kDisServiceUuid[] = "180A";
constexpr char kDisManufacturerUuid[] = "2A29";
constexpr char kDisModelUuid[] = "2A24";
constexpr char kDisSerialUuid[] = "2A25";
constexpr char kDisFirmwareUuid[] = "2A26";
constexpr char kDisHardwareUuid[] = "2A27";

constexpr uint8_t kReadingWireBytes = BleCodec::kReadingBytes;
constexpr uint8_t kStatusWireBytes = BleCodec::kStatusBytes;

// ArduinoBLE 2.1.0 hides a write's true length in two ways, and every write
// must reach BleCodec with it intact:
// - a fixed-length characteristic reports its full size after any write and
//   keeps the old tail bytes, so a short write would pass a length check.
//   Every writable characteristic is therefore variable length;
// - a write longer than the characteristic is truncated without an error, so
//   each writable characteristic has one spare byte: an over-long write
//   arrives one byte too long and is rejected.
constexpr uint8_t kSpareByte = 1;

// Protocol v5 authenticates at the application layer (PROTOCOL.md §3): no
// characteristic needs a link-layer encrypted link, because LE Secure
// Connections pairing is unreliable on this stack (MIC failures, hangs).
// Instead a phone proves a shared key through the auth characteristic, and
// until it has, every protected characteristic holds zeros and every write to
// one is dropped. ArduinoBLE cannot refuse a read per connection, so "blank
// until authenticated" is the gate.
constexpr uint16_t kProtected = 0;
constexpr uint8_t kMaxWriteBytes = BleCodec::kAuthProveBytes + kSpareByte;

BLEService service(kServiceUuid);
BLECharacteristic intervalCharacteristic(
    kIntervalUuid, BLERead | BLEWrite | kProtected, 4 + kSpareByte, false);
BLECharacteristic configCharacteristic(
    kConfigUuid, BLERead | BLEWrite | kProtected,
    BleCodec::kCoreConfigBytes + kSpareByte, false);
BLECharacteristic readingCharacteristic(
    kReadingUuid, BLERead | BLENotify | kProtected, kReadingWireBytes, true);
BLECharacteristic statusCharacteristic(
    kStatusUuid, BLERead | BLENotify | kProtected, kStatusWireBytes, true);
BLECharacteristic epochCharacteristic(
    kEpochUuid, BLERead | BLEWrite | kProtected, 8 + kSpareByte, false);
BLECharacteristic screenCharacteristic(
    kScreenUuid, BLERead | BLEWrite | kProtected, 1 + kSpareByte, false);
BLECharacteristic calibrationCharacteristic(
    kCalibrationUuid, BLERead | BLEWrite | kProtected,
    BleCodec::kCalibrationOffsetsBytes + kSpareByte, false);
BLECharacteristic nameCharacteristic(
    kNameUuid, BLERead | BLEWrite | kProtected,
    Config::kDeviceNameMaxLength + kSpareByte, false);
BLECharacteristic syncControlCharacteristic(
    kSyncControlUuid, BLEWrite | kProtected,
    BleCodec::kLogSyncControlBytes + kSpareByte, false);
BLECharacteristic logDataCharacteristic(
    kLogDataUuid, BLENotify | kProtected, BleCodec::kMaxLogPacketBytes, false);
BLECharacteristic calControlCharacteristic(
    kCalControlUuid, BLEWrite | kProtected,
    BleCodec::kCalibrationControlBytes + kSpareByte, false);
BLECharacteristic deviceInfoCharacteristic(kDeviceInfoUuid, BLERead,
                                           BleCodec::kDeviceInfoBytes, true);
BLECharacteristic calibrationStateCharacteristic(
    kCalibrationStateUuid, BLERead | kProtected,
    BleCodec::kCalibrationStateBytes, true);
BLECharacteristic diagnosticsCharacteristic(
    kDiagnosticsUuid, BLERead | kProtected, BleCodec::kDiagnosticsBytes, true);
// Sized for the longest opcode (log erase, 8 bytes); the codec checks each
// opcode's own length, so a 4-byte factory reset still arrives exact.
BLECharacteristic deviceControlCharacteristic(
    kDeviceControlUuid, BLEWrite | kProtected,
    BleCodec::kMaxDeviceControlBytes + kSpareByte, false);
BLECharacteristic authCharacteristic(kAuthUuid, BLERead | BLEWrite,
                                     BleCodec::kAuthProveBytes + kSpareByte,
                                     false);
BLECharacteristic enrolKeyCharacteristic(kEnrolKeyUuid, BLERead,
                                         BleCodec::kEnrolKeyBytes, true);
BLECharacteristic sleepWindowCharacteristic(
    kSleepWindowUuid, BLERead | BLEWrite | kProtected,
    BleCodec::kSleepWindowBytes + kSpareByte, false);

BLEService disService(kDisServiceUuid);
BLECharacteristic disManufacturer(kDisManufacturerUuid, BLERead,
                                  UnitIdentity::kManufacturer);
BLECharacteristic disModel(kDisModelUuid, BLERead, UnitIdentity::kModel);
BLECharacteristic disSerial(kDisSerialUuid, BLERead,
                            UnitIdentity::kSerialChars, true);
BLECharacteristic disFirmware(kDisFirmwareUuid, BLERead,
                              FirmwareVersion::kString);
BLECharacteristic disHardware(kDisHardwareUuid, BLERead,
                              UnitIdentity::kHardwareRevision);

// One slot per writable characteristic, so writes to different
// characteristics never displace each other; a second write to the same one
// before App::step() takes it replaces the first. Callbacks run inside
// BLE.poll() on the application thread, so the slots need no locking.
enum WriteSlot : uint8_t {
  kSlotCoreConfig,  // config slots in the order they are applied
  kSlotInterval,
  kSlotScreen,
  kSlotCalibration,
  kSlotName,
  kSlotEpoch,
  kSlotSyncControl,
  kSlotCalControl,
  kSlotDeviceControl,
  kSlotSleepWindow,
  kSlotAuth,  // handled by BleConfig::poll itself, never gated
  kSlotCount,
};

struct PendingWrite {
  uint8_t wire[kMaxWriteBytes];
  uint8_t length;
  bool pending;
};

PendingWrite pendingWrites[kSlotCount];

// Mirrors BleConfig::authenticated_ for the write callbacks.
bool linkAuthenticated = false;
BleConfig* activeAdapter = nullptr;
BLEDevice activePeer;

template <WriteSlot slot>
void written(BLEDevice device, BLECharacteristic characteristic) {
  if (!(device == activePeer)) return;
  if (slot != kSlotAuth && !linkAuthenticated) {
    return;  // unauthenticated writes change nothing
  }
  PendingWrite& write = pendingWrites[slot];
  const int length = characteristic.valueLength();
  write.length = static_cast<uint8_t>(
      length < 0 ? 0 : (length > kMaxWriteBytes ? kMaxWriteBytes : length));
  characteristic.readValue(write.wire, write.length);
  write.pending = true;
}

bool takeWrite(WriteSlot slot, uint8_t* wire, uint8_t& length) {
  PendingWrite& write = pendingWrites[slot];
  if (!write.pending) {
    return false;
  }
  length = write.length;
  memcpy(wire, write.wire, length);
  write.pending = false;
  return true;
}

bool decodeConfigWrite(WriteSlot slot, const uint8_t* wire, uint8_t length,
                       Config& config) {
  switch (slot) {
    case kSlotCoreConfig:
      return BleCodec::decodeCoreConfig(wire, length, config);
    case kSlotInterval:
      return BleCodec::decodeIntervalWrite(wire, length,
                                           config.measurementIntervalSeconds);
    case kSlotScreen:
      return BleCodec::decodeScreenWrite(wire, length, config.displayScreen);
    case kSlotCalibration:
      return BleCodec::decodeCalibrationOffsets(
          wire, length, config.noiseOffsetDb, config.tempOffsetC,
          config.humidityOffsetRh);
    case kSlotName:
      return BleCodec::decodeDeviceName(wire, length, config.deviceName);
    default:
      return false;
  }
}

// Enrolled phones: BondTable slots reused as {key id in the first two
// address bytes, shared key in the LTK}. Persisted by App like bonds were.
BondTable bondTable;
bool bondsChanged = false;
bool pairingOpen = false;  // enrolment open: the unit is on USB power

// The real value of every protected characteristic, written to the
// characteristic only while the connection is authenticated.
struct ProtectedValue {
  BLECharacteristic* characteristic;
  uint8_t value[BleCodec::kStatusBytes];
  uint8_t length;
};

ProtectedValue protectedValues[] = {
    {&intervalCharacteristic, {}, 4},
    {&configCharacteristic, {}, BleCodec::kCoreConfigBytes},
    {&readingCharacteristic, {}, BleCodec::kReadingBytes},
    {&statusCharacteristic, {}, BleCodec::kStatusBytes},
    {&epochCharacteristic, {}, 8},
    {&screenCharacteristic, {}, 1},
    {&calibrationCharacteristic, {}, BleCodec::kCalibrationOffsetsBytes},
    {&nameCharacteristic, {}, 0},
    {&calibrationStateCharacteristic, {}, BleCodec::kCalibrationStateBytes},
    {&diagnosticsCharacteristic, {}, BleCodec::kDiagnosticsBytes},
    {&sleepWindowCharacteristic, {}, BleCodec::kSleepWindowBytes},
};

void setProtected(BLECharacteristic& characteristic, const uint8_t* value,
                  uint8_t length) {
  for (ProtectedValue& entry : protectedValues) {
    if (entry.characteristic == &characteristic) {
      memcpy(entry.value, value, length);
      entry.length = length;
      if (linkAuthenticated) {
        characteristic.writeValue(value, length);
      }
      return;
    }
  }
}

void exposeProtected(bool show) {
  static const uint8_t kZeros[BleCodec::kStatusBytes] = {};
  for (ProtectedValue& entry : protectedValues) {
    entry.characteristic->writeValue(show ? entry.value : kZeros,
                                     entry.length);
  }
}

// Random bytes from the controller (HCI LE Rand), which draws on the
// nRF52840's hardware RNG. False when the controller does not answer: then
// no key or challenge is issued rather than a weak one.
bool randomBytes(uint8_t* out, uint8_t length) {
  uint8_t chunk[8];
  for (uint8_t offset = 0; offset < length; offset += sizeof chunk) {
    if (HCI.leRand(chunk) != 0) {
      return false;
    }
    const uint8_t remaining = static_cast<uint8_t>(length - offset);
    const uint8_t take =
        remaining < sizeof chunk ? remaining : static_cast<uint8_t>(sizeof chunk);
    memcpy(out + offset, chunk, take);
  }
  return true;
}

void keyIdAddress(uint16_t keyId, uint8_t address[6]) {
  memset(address, 0, 6);
  address[0] = static_cast<uint8_t>(keyId);
  address[1] = static_cast<uint8_t>(keyId >> 8);
}

namespace le = LittleEndian;

}  // namespace

bool BleConfig::begin(const Config& config, const char* serialNumber,
                      uint64_t nowMs) {
  current_ = config;
  if (!BLE.begin()) {
    return false;
  }
#if QUIESCO_BLE_TRACE
  BLE.debug(Serial);
#endif
  activeAdapter = this;
  BLE.setEventHandler(BLEConnected, onConnected);
  BLE.setEventHandler(BLEDisconnected, onDisconnected);
  resetSession(false);
  BLE.setLocalName(config.deviceName);
  BLE.setPairable(Pairable::NO);  // protocol v5: no link-layer pairing
  disService.addCharacteristic(disManufacturer);
  disService.addCharacteristic(disModel);
  disService.addCharacteristic(disSerial);
  disService.addCharacteristic(disFirmware);
  disService.addCharacteristic(disHardware);
  BLE.addService(disService);
  disSerial.writeValue(reinterpret_cast<const uint8_t*>(serialNumber),
                       strlen(serialNumber));
  BLE.setAdvertisedService(service);
  service.addCharacteristic(intervalCharacteristic);
  service.addCharacteristic(configCharacteristic);
  service.addCharacteristic(readingCharacteristic);
  service.addCharacteristic(statusCharacteristic);
  service.addCharacteristic(epochCharacteristic);
  service.addCharacteristic(screenCharacteristic);
  service.addCharacteristic(calibrationCharacteristic);
  service.addCharacteristic(nameCharacteristic);
  service.addCharacteristic(syncControlCharacteristic);
  service.addCharacteristic(logDataCharacteristic);
  service.addCharacteristic(calControlCharacteristic);
  service.addCharacteristic(deviceInfoCharacteristic);
  service.addCharacteristic(deviceControlCharacteristic);
  service.addCharacteristic(calibrationStateCharacteristic);
  service.addCharacteristic(diagnosticsCharacteristic);
  service.addCharacteristic(authCharacteristic);
  service.addCharacteristic(enrolKeyCharacteristic);
  service.addCharacteristic(sleepWindowCharacteristic);
  BLE.addService(service);
  intervalCharacteristic.setEventHandler(BLEWritten, written<kSlotInterval>);
  configCharacteristic.setEventHandler(BLEWritten, written<kSlotCoreConfig>);
  epochCharacteristic.setEventHandler(BLEWritten, written<kSlotEpoch>);
  screenCharacteristic.setEventHandler(BLEWritten, written<kSlotScreen>);
  calibrationCharacteristic.setEventHandler(BLEWritten,
                                            written<kSlotCalibration>);
  nameCharacteristic.setEventHandler(BLEWritten, written<kSlotName>);
  syncControlCharacteristic.setEventHandler(BLEWritten,
                                            written<kSlotSyncControl>);
  calControlCharacteristic.setEventHandler(BLEWritten,
                                           written<kSlotCalControl>);
  deviceControlCharacteristic.setEventHandler(BLEWritten,
                                              written<kSlotDeviceControl>);
  sleepWindowCharacteristic.setEventHandler(BLEWritten,
                                            written<kSlotSleepWindow>);
  authCharacteristic.setEventHandler(BLEWritten, written<kSlotAuth>);
  clearEnrolKey();
  newChallenge();
  publishConfig(config);
  publishSleepWindow(sleepWindow_);
  active_ = true;
  publishEpoch(epochMs_);
  advertising_ = BLE.advertise() != 0;
  applyAvailabilityPolicy(config, nowMs);
  return true;
}

void BleConfig::poll(uint64_t nowMs) {
  if (!active_) {
    return;
  }
  const unsigned long pollStartMs = millis();
  BLE.poll();
  const unsigned long pollMs = millis() - pollStartMs;
  if (pollMs > 50) {
    DebugLog::event("ble poll slow ms", static_cast<long>(pollMs));
  }
  if (challengePending_) {
    challengePending_ = false;
    newChallenge();
  }
  if (enrolKeyUntilMs_ != 0 && nowMs >= enrolKeyUntilMs_) {
    clearEnrolKey();
  }
  serviceAuth(nowMs);
  updateAdvertising();
}

void BleConfig::onConnected(BLEDevice device) {
  activePeer = device;
  if (activeAdapter) activeAdapter->resetSession(true);
}

void BleConfig::onDisconnected(BLEDevice device) {
  if (activeAdapter && device == activePeer) {
    activeAdapter->resetSession(false);
    activePeer = BLEDevice();
  }
}

void BleConfig::resetSession(bool connected) {
  ++session_;
  connected_ = connected;
  advertising_ = false;
  memset(pendingWrites, 0, sizeof pendingWrites);
  memset(challenge_, 0, sizeof challenge_);
  authFailures_ = 0;
  challengePending_ = connected;
  // Scrub inside the HCI event, before any following ATT request is handled.
  setAuthenticated(false);
  clearEnrolKey();
}

void BleConfig::setAuthenticated(bool authenticated) {
  if (authenticated != authenticated_) {
    DebugLog::event("ble authenticated", authenticated ? 1 : 0);
  }
  authenticated_ = authenticated;
  linkAuthenticated = authenticated;
  exposeProtected(authenticated);
  publishAuthState();
}

void BleConfig::newChallenge() {
  const uint32_t session = session_;
  uint8_t challenge[sizeof challenge_] = {};
  if (!randomBytes(challenge, sizeof challenge)) {
    memset(challenge, 0, sizeof challenge);
  }
  // HCI commands can service a disconnect/reconnect while waiting for RNG.
  if (session != session_) return;
  memcpy(challenge_, challenge, sizeof challenge_);
  publishAuthState();
}

void BleConfig::publishAuthState() {
  BleCodec::AuthState state;
  state.authenticated = authenticated_;
  state.enrolmentOpen = pairingOpen;
  state.enrolled = bondTable.count() > 0;
  state.challenge = challenge_;
  uint8_t out[BleCodec::kAuthStateBytes];
  BleCodec::encodeAuthState(state, out);
  authCharacteristic.writeValue(out, sizeof out);
}

void BleConfig::clearEnrolKey() {
  static const uint8_t kZeros[BleCodec::kEnrolKeyBytes] = {};
  enrolKeyCharacteristic.writeValue(kZeros, sizeof kZeros);
  enrolKeyUntilMs_ = 0;
  pendingKeyId_ = 0;
  pendingCode_ = 0;
  memset(pendingSetupKey_, 0, sizeof pendingSetupKey_);
  memset(pendingKey_, 0, sizeof pendingKey_);
  enrollmentScreenPending_ = false;
}

void BleConfig::serviceAuth(uint64_t nowMs) {
  uint8_t wire[kMaxWriteBytes];
  uint8_t length = 0;
  if (!takeWrite(kSlotAuth, wire, length)) {
    return;
  }
  BleCodec::AuthRequest request;
  if (!connected_ || !BleCodec::decodeAuthWrite(wire, length, request)) {
    publishAuthState();  // restore the readable state after a bad write
    return;
  }
  if (request.opcode == BleCodec::kAuthOpEnrol) {
    enrol(nowMs);
    return;
  }
  const uint32_t session = session_;
  // Prove. A handful of tries per connection; each consumes the challenge.
  static const uint8_t kZeroChallenge[BleCodec::kAuthChallengeBytes] = {};
  bool accepted = false;
  if (authFailures_ < kMaxAuthFailures &&
      !HmacSha256::equal(challenge_, kZeroChallenge, sizeof challenge_)) {
    uint8_t address[6];
    uint8_t key[BleCodec::kAuthKeyBytes];
    keyIdAddress(request.keyId, address);
    const bool pending = pendingKeyId_ != 0 && request.keyId == pendingKeyId_ &&
                         pairingOpen && nowMs < enrolKeyUntilMs_;
    if (pending) memcpy(key, pendingSetupKey_, sizeof key);
    if (pending || bondTable.findLtk(address, key)) {
      uint8_t expected[BleCodec::kAuthProofBytes];
      accepted =
          BleCodec::authProof(key, challenge_, request.keyId, expected) &&
          HmacSha256::equal(expected, request.proof, sizeof expected);
    }
  }
  DebugLog::event("ble auth attempt", accepted ? 1 : 0);
  if (!accepted && authFailures_ < kMaxAuthFailures) {
    authFailures_++;
  }
  newChallenge();
  if (accepted && connected_ && session == session_) {
    if (pendingKeyId_ != 0 && request.keyId == pendingKeyId_) {
      // The code only admits this setup; the phone keeps a fresh full-length
      // key, so recording a later connection reveals nothing guessable.
      uint8_t address[6];
      keyIdAddress(pendingKeyId_, address);
      bondTable.storeLtk(address, pendingKey_);
      bondsChanged = true;  // only a proof of the on-screen code can add a phone
      uint8_t out[BleCodec::kEnrolKeyBytes];
      BleCodec::encodeEnrolKey(pendingKeyId_, pendingKey_, out);
      clearEnrolKey();
      enrolKeyCharacteristic.writeValue(out, sizeof out);
      memset(out, 0, sizeof out);
      enrolKeyUntilMs_ = nowMs + kIssuedKeyVisibleMs;
    }
    setAuthenticated(true);
  }
}

void BleConfig::enrol(uint64_t nowMs) {
  if (!pairingOpen) {
    DebugLog::event("ble enrol refused (not on USB)");
    publishAuthState();
    return;
  }
  if (pendingKeyId_ != 0 || authFailures_ >= kMaxAuthFailures) {
    DebugLog::event("ble enrol ignored (pending or locked out)");
    publishAuthState();
    return;
  }
  const uint32_t session = session_;
  uint8_t key[BleCodec::kAuthKeyBytes];
  uint8_t idBytes[2];
  uint32_t code = BleCodec::kSetupCodeCount;
  // Rejection sampling keeps every code equally likely.
  constexpr uint32_t kCodeLimit =
      UINT32_MAX - UINT32_MAX % BleCodec::kSetupCodeCount;
  for (int attempt = 0; attempt < 8 && code >= BleCodec::kSetupCodeCount;
       attempt++) {
    uint8_t raw[4];
    if (!randomBytes(raw, sizeof raw)) break;
    const uint32_t value = LittleEndian::getU32(raw);
    if (value < kCodeLimit) code = value % BleCodec::kSetupCodeCount;
  }
  if (code >= BleCodec::kSetupCodeCount) {
    DebugLog::event("ble enrol failed (no random)");
    publishAuthState();
    return;
  }
  uint16_t keyId = 0;
  uint8_t address[6];
  uint8_t existing[BleCodec::kAuthKeyBytes];
  for (int attempt = 0; attempt < 8 && keyId == 0; attempt++) {
    if (!randomBytes(idBytes, sizeof idBytes) || !randomBytes(key, sizeof key)) {
      DebugLog::event("ble enrol failed (no random)");
      publishAuthState();
      return;
    }
    keyId = static_cast<uint16_t>(idBytes[0] | (idBytes[1] << 8));
    keyIdAddress(keyId, address);
    if (bondTable.findLtk(address, existing)) {
      keyId = 0;  // taken: draw again
    }
  }
  if (keyId == 0 || !connected_ || session != session_ || !pairingOpen) {
    publishAuthState();
    return;
  }
  if (!BleCodec::setupKey(code, keyId, pendingSetupKey_)) {
    DebugLog::event("ble enrol failed (no hmac)");
    memset(pendingSetupKey_, 0, sizeof pendingSetupKey_);
    publishAuthState();
    return;
  }
  pendingKeyId_ = keyId;
  pendingCode_ = code;
  memcpy(pendingKey_, key, sizeof pendingKey_);
  enrollmentScreenPending_ = true;
  uint8_t out[BleCodec::kEnrolKeyBytes];
  BleCodec::encodeEnrolKey(keyId, nullptr, out);
  enrolKeyCharacteristic.writeValue(out, sizeof out);
  enrolKeyUntilMs_ = nowMs + kEnrolKeyVisibleMs;
  DebugLog::event("ble pending setup id", keyId);
  publishAuthState();
}

void BleConfig::setBonds(const BondTable& bonds) {
  bondTable = bonds;
  bondsChanged = false;
}

bool BleConfig::takeBondsChanged(BondTable& bonds) {
  if (!bondsChanged) {
    return false;
  }
  bondsChanged = false;
  bonds = bondTable;
  return true;
}

void BleConfig::clearBonds() {
  bondTable.clear();
  clearEnrolKey();
  bondsChanged = true;
}

uint8_t BleConfig::bondCount() const {
  return bondTable.count();
}

void BleConfig::disconnect() {
  if (active_ && connected_) {
    BLE.disconnect();
  }
}

void BleConfig::setPairingAllowed(bool allowed) {
  if (allowed == pairingOpen) {
    return;
  }
  pairingOpen = allowed;
  if (!allowed) clearEnrolKey();
  if (active_) {
    publishAuthState();  // the enrolment-open flag
  }
  updateAdvertising();  // plugging in or out changes the "plugged in" policy
}

bool BleConfig::pairingAllowed() const {
  return pairingOpen;
}

bool BleConfig::takeEnrollment(uint32_t& code) {
  if (!enrollmentScreenPending_ || pendingKeyId_ == 0) return false;
  enrollmentScreenPending_ = false;
  code = pendingCode_;
  return true;
}

bool BleConfig::takePendingConfig(Config& config) {
  Config candidate = current_;
  bool taken = false;
  bool applied = false;
  for (uint8_t slot = kSlotCoreConfig; slot <= kSlotName; slot++) {
    uint8_t wire[kMaxWriteBytes];
    uint8_t length = 0;
    if (!takeWrite(static_cast<WriteSlot>(slot), wire, length)) {
      continue;
    }
    taken = true;
    Config trial = candidate;
    if (decodeConfigWrite(static_cast<WriteSlot>(slot), wire, length, trial) &&
        isValidConfig(trial)) {
      candidate = trial;
      applied = true;
    }
  }
  if (!taken) {
    return false;
  }
  // Republishing every config characteristic also reverts any rejected one,
  // so a read-back always shows what the device applied.
  publishConfig(applied ? candidate : current_);
  if (applied) {
    config = candidate;
  }
  return applied;
}

bool BleConfig::takePendingEpoch(uint64_t& epochMs) {
  uint8_t wire[kMaxWriteBytes];
  uint8_t length = 0;
  if (!takeWrite(kSlotEpoch, wire, length)) {
    return false;
  }
  if (!BleCodec::decodeEpochWrite(wire, length, epochMs)) {
    publishEpoch(epochMs_);  // revert to the last accepted value
    return false;
  }
  return true;
}

bool BleConfig::takePendingSyncRequest(BleCodec::LogSyncRequest& request) {
  uint8_t wire[kMaxWriteBytes];
  uint8_t length = 0;
  return takeWrite(kSlotSyncControl, wire, length) &&
         BleCodec::decodeLogSyncControl(wire, length, request);
}

bool BleConfig::takePendingFrcRequest(uint16_t& targetPpm) {
  uint8_t wire[kMaxWriteBytes];
  uint8_t length = 0;
  return takeWrite(kSlotCalControl, wire, length) &&
         BleCodec::decodeCalibrationControl(wire, length, targetPpm);
}

bool BleConfig::takePendingDeviceControl(
    BleCodec::DeviceControlRequest& request) {
  uint8_t wire[kMaxWriteBytes];
  uint8_t length = 0;
  return takeWrite(kSlotDeviceControl, wire, length) &&
         BleCodec::decodeDeviceControl(wire, length, request);
}

bool BleConfig::takePendingSleepWindow(SleepWindow& window) {
  uint8_t wire[kMaxWriteBytes];
  uint8_t length = 0;
  if (!takeWrite(kSlotSleepWindow, wire, length)) {
    return false;
  }
  SleepWindow candidate = sleepWindow_;
  if (!BleCodec::decodeSleepWindow(wire, length, candidate)) {
    publishSleepWindow(sleepWindow_);  // revert to the value in use
    return false;
  }
  window = candidate;
  return true;
}

bool BleConfig::notifyLogData(const uint8_t* data, uint16_t length) {
  if (!active_ || !authenticated_ || !logDataCharacteristic.subscribed()) {
    return false;
  }
  return logDataCharacteristic.writeValue(data, length) != 0;
}

void BleConfig::publishConfig(const Config& config) {
  const bool nameChanged =
      strncmp(current_.deviceName, config.deviceName, sizeof config.deviceName) !=
      0;
  current_ = config;
  uint8_t interval[4];
  uint8_t encoded[BleCodec::kCoreConfigBytes];
  le::putU32(interval, config.measurementIntervalSeconds);
  BleCodec::encodeCoreConfig(config, encoded);
  setProtected(intervalCharacteristic, interval, sizeof interval);
  setProtected(configCharacteristic, encoded, sizeof encoded);
  setProtected(screenCharacteristic, &config.displayScreen, 1);
  uint8_t calibration[BleCodec::kCalibrationOffsetsBytes];
  BleCodec::encodeCalibrationOffsets(config, calibration);
  setProtected(calibrationCharacteristic, calibration, sizeof calibration);
  publishCalibrationState();
  setProtected(nameCharacteristic,
               reinterpret_cast<const uint8_t*>(config.deviceName),
               static_cast<uint8_t>(strlen(config.deviceName)));
  if (active_ && nameChanged) {
    const bool restartAdvertising = advertising_ && !connected_;
    if (restartAdvertising) {
      BLE.stopAdvertise();
      advertising_ = false;
    }
    BLE.setLocalName(config.deviceName);
    if (restartAdvertising) {
      advertising_ = BLE.advertise() != 0;
    }
  }
}

void BleConfig::publishDiagnostics(const BleCodec::Diagnostics& diagnostics) {
  uint8_t out[BleCodec::kDiagnosticsBytes];
  BleCodec::encodeDiagnostics(diagnostics, out);
  setProtected(diagnosticsCharacteristic, out, sizeof out);
}

void BleConfig::publishAscDisabled(bool ascDisabled) {
  ascDisabled_ = ascDisabled;
  publishCalibrationState();
}

void BleConfig::publishCalibrationState() {
  uint8_t out[BleCodec::kCalibrationStateBytes];
  BleCodec::encodeCalibrationState(current_, ascDisabled_, out);
  setProtected(calibrationStateCharacteristic, out, sizeof out);
}

void BleConfig::publishDeviceInfo(const BleCodec::DeviceInfo& info) {
  uint8_t out[BleCodec::kDeviceInfoBytes];
  BleCodec::encodeDeviceInfo(info, out);
  deviceInfoCharacteristic.writeValue(out, sizeof out);
}

void BleConfig::publishReading(const Reading& reading) {
  if (!active_) return;
  uint8_t out[BleCodec::kReadingBytes];
  BleCodec::encodeReadingCompact(reading, out);
  setProtected(readingCharacteristic, out, sizeof out);  // notifies if authenticated
}

void BleConfig::publishEpoch(uint64_t epochMs) {
  epochMs_ = epochMs;
  if (!active_) return;
  uint8_t out[8];
  LittleEndian::putU64(out, epochMs);
  setProtected(epochCharacteristic, out, sizeof out);
}

void BleConfig::publishSleepWindow(const SleepWindow& window) {
  sleepWindow_ = window;
  uint8_t out[BleCodec::kSleepWindowBytes];
  BleCodec::encodeSleepWindow(window, out);
  setProtected(sleepWindowCharacteristic, out, sizeof out);
}

void BleConfig::publishStatus(const BleCodec::StatusInfo& info) {
  if (!active_) return;
  uint8_t out[BleCodec::kStatusBytes];
  BleCodec::encodeStatus(info, out);
  setProtected(statusCharacteristic, out, sizeof out);
}

void BleConfig::applyAvailabilityPolicy(const Config& config, uint64_t nowMs) {
  (void)nowMs;
  alwaysAvailable_ = config.bleAlwaysAvailable;
  updateAdvertising();
}

// "Plugged in": advertise only on USB power, the same condition that opens
// enrolment, so a unit on battery stays silent and saves its radio current.
void BleConfig::updateAdvertising() {
  if (!active_ || connected_) {
    return;
  }
  const bool shouldAdvertise = alwaysAvailable_ || pairingOpen;
  if (shouldAdvertise && !advertising_) {
    advertising_ = BLE.advertise() != 0;
  } else if (!shouldAdvertise && advertising_) {
    BLE.stopAdvertise();
    advertising_ = false;
  }
}
