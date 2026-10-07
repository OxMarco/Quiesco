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
  void publishCalibrationState();
  void setAuthenticated(bool authenticated);
  void newChallenge();
  void publishAuthState();
  void clearEnrolKey();
  void serviceAuth(uint64_t nowMs);
  void enrol(uint64_t nowMs);

  static constexpr uint8_t kMaxAuthFailures = 5;
  static constexpr uint32_t kEnrolKeyVisibleMs = 180000;
  // How long the issued key stays in 0011 for the phone to read.
  static constexpr uint32_t kIssuedKeyVisibleMs = 30000;

  Config current_ = defaultConfig();
  SleepWindow sleepWindow_ = defaultSleepWindow();
  uint64_t epochMs_ = 0;
  bool ascDisabled_ = false;  // last accepted epoch write; 0 = never synced
  bool active_ = false;
  bool advertising_ = false;
  bool connected_ = false;
  bool authenticated_ = false;
  uint8_t challenge_[BleCodec::kAuthChallengeBytes] = {};
  uint8_t authFailures_ = 0;
  uint32_t session_ = 0;
  bool challengePending_ = false;
  uint16_t pendingKeyId_ = 0;
  uint32_t pendingCode_ = 0;
  uint8_t pendingSetupKey_[BleCodec::kAuthKeyBytes] = {};  // derived from the code
  uint8_t pendingKey_[BleCodec::kAuthKeyBytes] = {};  // issued once the code is proved
  bool enrollmentScreenPending_ = false;
  uint64_t enrolKeyUntilMs_ = 0;  // 0 = no pending setup
  bool alwaysAvailable_ = true;
};
