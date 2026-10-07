// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

#include "board/PowerDomain.h"
#include "drivers/BatteryMonitor.h"
#include "drivers/EpaperDisplay.h"
#include "drivers/PdmMicrophone.h"
#include "drivers/W25Q64Flash.h"
#include "model/Config.h"
#include "model/FaultStatus.h"
#include "model/Reading.h"
#include "model/SleepWindow.h"
#include "platform/MonotonicClock.h"
#include "platform/HardwareWatchdog.h"
#include "drivers/BleConfig.h"
#include "services/ConfigStore.h"
#include "services/EnvironmentalSampler.h"
#include "services/BondTable.h"
#include "services/SampleLog.h"
#include "services/Scheduler.h"
#include "services/UnitIdentity.h"
#include "services/WallClock.h"
#include "ui/UiModel.h"

class App {
 public:
  void begin();
  void step();

 private:
  enum class State : uint8_t {
    kIdle,
    kPowering,
    kInitializing,
    kAcquiring,
    kWaiting,
    kCollecting,
    kFrcSoaking,
    kFrcExecuting,
    kPersisting,
    kErasingLog,
    kRendering,
    kSyncing,
    kShuttingDown,
    kCommittingConfig,
    kRedrawing,
    kDumping,
    kStressWriting,
    kTraceDumping,
  };

  // What a rail power-up is for. A redraw repaints the last reading after a
  // screen or calibration change without the ~7 s measurement; a config
  // commit only writes flash; a sync only streams the log.
  enum class CycleKind : uint8_t {
    kMeasure,
    kCommitConfig,
    kRedraw,
    kDump,
    kStress,
    kTraceDump,
    kSync,
  };

  // FRC run-up per Sensirion's low-power AN: single shots once a minute for
  // 5 min, as in the field; the soak bound adds margin for the last shot.
  static constexpr uint32_t kFrcSoakMs = 330000;
  static constexpr uint32_t kSyncProgressTimeoutMs = 10000;
  static constexpr uint8_t kSyncBatchMax = 3;
  // Minimum gap between log notifications. ArduinoBLE's notify always
  // reports success and the stack drops packets under load, so the stream is
  // paced instead. Measured from a Mac (2026-10-07, MTU 247, 3 records a
  // packet): 0 ms lost ~45 % of records, 15-20 ms lost 4-35 % and once
  // stalled, 30 ms lost none at 54 records/s. The app still checks the packet
  // counter and asks again from the first missing record (PROTOCOL.md 7.4).
#ifndef QUIESCO_SYNC_GAP_MS
#define QUIESCO_SYNC_GAP_MS 30
#endif
  static constexpr uint32_t kSyncPacketGapMs = QUIESCO_SYNC_GAP_MS;

  void startCycle(uint64_t nowMs, CycleKind kind);
  void commitConfig(uint64_t nowMs);
  bool loadSettings(uint64_t nowMs);
  void applyBleConfig(uint64_t nowMs);
  void takeBleCommands(uint64_t nowMs);
  void factoryReset(uint64_t nowMs, bool eraseLog);
  void requestLogErase(uint64_t nowMs, uint32_t upToSequence);
  void startLogErase();
  void applySleepWindow(uint64_t nowMs);
  JudgeMode judgeModeAt(uint64_t nowMs) const;
  bool updateAllowed() const;
  bool readyForUpdate() const;
  bool settingsSavePending() const {
    return configSavePending_ || bondsSavePending_ || sleepSavePending_;
  }
  void finishPersisting(bool flashPresent);
  bool saveSettings(uint64_t nowMs);
  void servicePairing(uint64_t nowMs);
  void publishDeviceInfo();
  void publishDiagnostics(uint64_t nowMs);
  void handleConsole(uint64_t nowMs);
  void serviceDump();
  void serviceStress(uint64_t nowMs);
  static uint32_t stateLimitMs(State state);
  void startPendingSync(uint64_t nowMs);
  void beginSyncSession(uint64_t nowMs);
  void serviceSync(uint64_t nowMs);
  void finishSync();
  uint16_t syncRemainingRecords() const;
  void publishStatus();
  // Debounced ~CHG; always false on a --no-battery build.
  bool pollCharging(uint64_t nowMs);

  MonotonicClock clock_;
  WallClock wallClock_;
  HardwareWatchdog watchdog_;
  PowerDomain power_;
  Scheduler scheduler_;
  EnvironmentalSampler environmentalSampler_;
  PdmMicrophone microphone_;
  BatteryMonitor batteryMonitor_;
  W25Q64Flash flash_;
  SampleLog sampleLog_;
  ConfigStore configStore_;
  BleConfig ble_;
  EpaperDisplay display_;
  uint64_t deviceId_ = 0;
  char serialNumber_[UnitIdentity::kSerialChars + 1] = {};
  uint64_t publishedScd41Serial_ = 0;
  Config config_ = defaultConfig();
  Reading reading_;
  FaultStatus faults_;
  UiModel lastRendered_;
  State state_ = State::kIdle;
  unsigned long cycleNumber_ = 0;
  bool hasRendered_ = false;
  uint32_t partialRefreshesSinceFull_ = 0;
  uint64_t nextConfigRetryMs_ = 0;
  bool configLoaded_ = false;
  bool configSavePending_ = false;
  CycleKind cycleKind_ = CycleKind::kMeasure;
  bool sampleRequested_ = false;
  // The last reading before calibration, so a redraw can re-apply changed
  // offsets; hasReading_ until the first cycle has collected one.
  Reading rawReading_;
  bool hasReading_ = false;
  bool redrawRequested_ = false;
  uint32_t bootCount_ = 0;
  uint32_t resetReason_ = 0;
  // Debug console log dump (debug builds only): its own rail cycle.
  static constexpr uint8_t kDumpRecordsPerStep = 16;
  bool dumpRequested_ = false;
  bool traceDumpRequested_ = false;
  bool dumpStarted_ = false;
  uint32_t dumpCount_ = 0;
  SampleLogCursor dumpCursor_;
  // Power-pull test (debug console 'w'): back-to-back config saves and log
  // appends, so unplugging lands mid-write. Stays under the state bound.
  static constexpr uint32_t kStressMs = 45000;
  bool stressRequested_ = false;
  bool stressStarted_ = false;
  uint64_t stressEndMs_ = 0;
  uint32_t stressOps_ = 0;
  uint32_t stressFailures_ = 0;
  // Progress watchdog: the hardware watchdog is fed only while the current
  // state has lasted less than its bound (stateLimitMs), so a state that
  // stops advancing resets the unit even though step() keeps running.
  State watchedState_ = State::kIdle;
  uint64_t stateSinceMs_ = 0;
  // Bonds are persisted like config: the adapter hands over a changed table,
  // and the next rail cycle writes it.
  BondTable bonds_;
  bool bondsSavePending_ = false;
  // Setup: the secret stays on the panel until its proof succeeds, the
  // phone leaves, or kPairingScreenMs passes.
  static constexpr uint32_t kPairingScreenMs = 180000;
  bool pairingShown_ = false;
  uint32_t pairingCode_ = 0;
  uint64_t pairingUntilMs_ = 0;
  bool publishedPairingOpen_ = false;
  bool publishedBonded_ = false;
  bool publishedAscDisabled_ = false;
  // The sleep window (PROTOCOL.md §6.17), persisted under its own key.
  SleepWindow sleepWindow_ = defaultSleepWindow();
  bool sleepSavePending_ = false;
  // Factory reset or the erase command: the log erase rides the next cycle's
  // persisting step. An erase command that arrives before the log is mounted
  // waits in logEraseRequested_ until saveSettings() can check its up-to
  // sequence against the newest record.
  bool logEraseRequested_ = false;
  uint32_t logEraseUpTo_ = 0;
  bool logErasePending_ = false;
  uint32_t logEraseSequence_ = 0;
  bool logEraseCompleted_ = false;
  uint32_t bleSession_ = 0;
  bool persistConfigSaved_ = false;
  // SCD41 forced recalibration: one-shot, rides a measurement cycle.
  bool frcPending_ = false;
  // Device control opcode 3: reboot into the bootloader's OTA mode once idle
  // with nothing left to write; the link is dropped first so the phone sees
  // a clean disconnect, then the reset follows kUpdateDisconnectMs later.
  static constexpr uint32_t kUpdateDisconnectMs = 300;
  static constexpr float kUpdateMinBatteryVolts = 3.6f;
  bool updateRequested_ = false;
  bool usbUpdate_ = false;  // opcode 4: the XIAO-BOOT drive, not BLE OTA
  uint64_t updateResetAtMs_ = 0;
  uint16_t frcTargetPpm_ = 0;
  uint8_t frcState_ = BleCodec::kFrcIdle;  // reported via status
  int16_t frcCorrectionPpm_ = 0;
  uint64_t frcDeadlineMs_ = 0;
  // Log sync session (kSyncing); the cursor only advances after a queued
  // notification, so an unsent packet is rebuilt and retried.
  BleCodec::LogSyncRequest syncRequest_ = {};
  SampleLogCursor syncCursor_;
  bool syncPending_ = false;
  bool syncAbort_ = false;
  bool syncActive_ = false;
  bool syncEndPending_ = false;
  uint8_t syncPacketSeq_ = 0;
  uint8_t syncBatchCount_ = 0;
  uint8_t syncFragIndex_ = 0;
  uint16_t syncSentRecords_ = 0;
  uint16_t syncAttPayload_ = 0;
  uint32_t lastSentSequence_ = 0;
  uint64_t syncDeadlineMs_ = 0;
  uint64_t syncLastSendMs_ = 0;
  SampleRecord syncBatch_[kSyncBatchMax];
  uint8_t syncWire_[BleCodec::kWireRecordBytes];
  uint8_t syncPacket_[BleCodec::kMaxLogPacketBytes];
  // Charger state the current screen reflects; a stable change while idle
  // triggers an immediate cycle instead of waiting out the interval.
  bool renderedCharging_ = false;
};
