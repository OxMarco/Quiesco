// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

#include "../model/Config.h"
#include "../model/SleepWindow.h"
#include "../protocol/BleCodec.h"

class BondTable;
class BLEDevice;
struct Reading;

// Allocation-free Quiesco GATT adapter. Callbacks only copy bounded wire data;
// validation and application happen from App::step().
class BleConfig {
 public:
  // serialNumber is the DIS serial string; it must outlive the call only.
  bool begin(const Config& config, const char* serialNumber, uint64_t nowMs);
  void poll(uint64_t nowMs);
  bool takePendingConfig(Config& config);
  bool takePendingEpoch(uint64_t& epochMs);
  bool takePendingSyncRequest(BleCodec::LogSyncRequest& request);
  bool takePendingFrcRequest(uint16_t& targetPpm);
  // Factory reset or log erase (PROTOCOL.md §6.11).
  bool takePendingDeviceControl(BleCodec::DeviceControlRequest& request);
  // A valid write to 0012; an invalid one republishes the value in use.
  bool takePendingSleepWindow(SleepWindow& window);

  // Also republishes the calibration state, which reads the FRC history
  // from config.
  void publishConfig(const Config& config);
  void publishAscDisabled(bool ascDisabled);
  // Read-only, no notify; valid before begin() like publishConfig.
  void publishDiagnostics(const BleCodec::Diagnostics& diagnostics);
  // 0013, a ResetHistory record (all zeros: none). Valid before begin().
  void publishLastReset(const uint8_t record[BleCodec::kLastResetBytes]);
  // Like publishConfig, valid before begin(): it only sets the local value.
  void publishDeviceInfo(const BleCodec::DeviceInfo& info);
  void publishReading(const Reading& reading);
  void publishStatus(const BleCodec::StatusInfo& info);
  void publishEpoch(uint64_t epochMs);
  // Like publishConfig, valid before begin(): it only sets the local value.
  void publishSleepWindow(const SleepWindow& window);
  // False when disconnected, unauthenticated, unsubscribed, or the stack could
  // not queue the notification (backpressure) — the caller retries.
  bool notifyLogData(const uint8_t* data, uint16_t length);
  void applyAvailabilityPolicy(const Config& config, uint64_t nowMs);

  // Bonds live in the adapter (ArduinoBLE asks for keys mid-poll). App loads
  // them from flash once, and persists them whenever pairing changed them.
  void setBonds(const BondTable& bonds);
  bool takeBondsChanged(BondTable& bonds);
  void clearBonds();
  uint8_t bondCount() const;

  // USB gate: enrollment also needs proof of the on-screen code.
  void setPairingAllowed(bool allowed);
  bool pairingAllowed() const;
  // Six-digit setup code for the e-ink screen. Never sent over BLE.
  bool takeEnrollment(uint32_t& code);
  bool enrollmentPending() const { return pendingKeyId_ != 0; }

  bool active() const { return active_; }
  bool connected() const { return connected_; }
  // Drops the current link, if any (before a reboot into the bootloader).
  void disconnect();
  // This connection proved an enrolled key (PROTOCOL.md §3).
  bool authenticated() const { return authenticated_; }
  uint32_t session() const { return session_; }

 private:
  static void onConnected(BLEDevice device);
  static void onDisconnected(BLEDevice device);
  void resetSession(bool connected);
  void updateAdvertising();
  void setAdvertisedName(const char* name);
  void publishCalibrationState();
  void setAuthenticated(bool authenticated);
  void newChallenge();
  void publishAuthState();
  void clearEnrolKey();
  void serviceAuth(uint64_t nowMs);
  void enrol(uint64_t nowMs);

  void dropIdleLink(uint64_t nowMs);

  static constexpr uint8_t kMaxAuthFailures = 5;
  // The unit takes one connection and stops advertising while it has one, so
  // a link that proves no key would keep the owner's phone out for as long
  // as it stayed. It is dropped after this long, or at the last failed
  // proof. The app proves its key, or starts a setup, within a second or two
  // of connecting.
  static constexpr uint32_t kUnauthenticatedLinkMs = 30000;
  // A setup in progress (someone is typing the code from the panel) keeps
  // the link for the setup's own lifetime, but no connection stays
  // unauthenticated longer than this in total, however many setups it asks
  // for.
  static constexpr uint32_t kSetupLinkMs = 240000;
  // Setup codes issued without a completed setup before ENROL is refused.
  // Failures reset with every connection, so without this, reconnecting
  // gave unlimited guesses. The allowance comes back one code per
  // kSetupCodeRefillMs, and at once when USB is unplugged and plugged back
  // in: a stranger in range asking for ten codes must not lock the owner out
  // of setup for as long as the unit stays plugged in. At five guesses a
  // code, that is 120 guesses a day in a million.
  static constexpr uint8_t kMaxSetupCodes = 10;
  static constexpr uint32_t kSetupCodeRefillMs = 60UL * 60 * 1000;
  static constexpr uint32_t kEnrolKeyVisibleMs = 180000;
  // How long the issued key stays in 0011 for the phone to read.
  static constexpr uint32_t kIssuedKeyVisibleMs = 30000;

  Config current_ = defaultConfig();
  SleepWindow sleepWindow_ = defaultSleepWindow();
  uint64_t epochMs_ = 0;
  bool ascDisabled_ = false;  // last accepted epoch write; 0 = never synced
  bool active_ = false;
  bool advertising_ = false;  // ArduinoBLE's flag, kept through connections
  bool refreshAdvertising_ = false;  // set at a disconnect
  bool connected_ = false;
  bool linkTimerPending_ = false;  // set in the connect callback
  uint64_t connectedSinceMs_ = 0;
  bool authenticated_ = false;
  uint8_t challenge_[BleCodec::kAuthChallengeBytes] = {};
  uint8_t authFailures_ = 0;
  uint8_t setupCodesIssued_ = 0;  // not yet given back (kSetupCodeRefillMs)
  uint64_t setupRefillFromMs_ = 0;  // start of the hour that returns the next
  uint32_t session_ = 0;
  bool challengePending_ = false;
  uint16_t pendingKeyId_ = 0;
  uint32_t pendingCode_ = 0;
  uint8_t pendingSetupKey_[BleCodec::kAuthKeyBytes] = {};  // derived from the code
  uint8_t pendingKey_[BleCodec::kAuthKeyBytes] = {};  // issued once the code is proved
  bool enrollmentScreenPending_ = false;
  uint64_t enrolKeyUntilMs_ = 0;  // 0 = no pending setup
  bool alwaysAvailable_ = true;
  // ArduinoBLE keeps the local name's pointer, reads it on every advertise()
  // and measures the previous name through it on the next set; each name
  // goes in the slot not in use, so neither is overwritten while referenced.
  char advertisedNames_[2][sizeof Config::deviceName] = {};
  uint8_t advertisedSlot_ = 0;
};
