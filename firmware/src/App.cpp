// SPDX-License-Identifier: GPL-3.0-only
#include "App.h"

#include <Arduino.h>
#include <string.h>

#include "diagnostics/BuildConfig.h"
#include "diagnostics/DebugLog.h"
#include "diagnostics/FirmwareVersion.h"
#include "platform/BootloaderEntry.h"
#include "platform/DeviceId.h"
#include "platform/ResetReason.h"
#include "services/CalibrationPolicy.h"
#include "storage/TraceStore.h"
#include "ui/Renderer.h"
#include "ui/SleepSchedule.h"

void App::begin() {
  power_.beginOff();
  batteryMonitor_.begin();
  deviceId_ = DeviceId::read();
  UnitIdentity::formatSerial(deviceId_, serialNumber_);
  resetReason_ = ResetReason::take();
  DebugLog::begin(FirmwareVersion::kString, serialNumber_,
                  ResetReason::describe(resetReason_));
  watchdog_.begin();

  config_ = UnitIdentity::defaultConfigForUnit(deviceId_);
  publishDeviceInfo();

  const uint64_t nowMs = clock_.nowMs();
  publishDiagnostics(nowMs);
  renderedCharging_ = pollCharging(nowMs);
  scheduler_.begin(nowMs, config_.measurementIntervalSeconds);
  if (scheduler_.claimDue(nowMs)) {
    startCycle(nowMs, CycleKind::kMeasure);
  }
}

void App::step() {
  const uint64_t nowMs = clock_.nowMs();
  const State stepState = state_;
  if (state_ != watchedState_) {
    watchedState_ = state_;
    stateSinceMs_ = nowMs;
  }
  if (state_ == State::kIdle || nowMs - stateSinceMs_ < stateLimitMs(state_)) {
    watchdog_.kick();
  }
  ble_.poll(nowMs);
  if (bleSession_ != ble_.session()) {
    bleSession_ = ble_.session();
    syncPending_ = false;
    syncAbort_ = syncActive_;
  }
  uint64_t epochMs = 0;
  if (ble_.takePendingEpoch(epochMs)) {
    wallClock_.sync(epochMs, nowMs);
    ble_.publishEpoch(epochMs);
  }
  if (configLoaded_) {
    applyBleConfig(nowMs);
    takeBleCommands(nowMs);
    servicePairing(nowMs);
    handleConsole(nowMs);
  }

  switch (state_) {
    case State::kIdle:
      if (updateResetAtMs_ != 0) {
        if (nowMs >= updateResetAtMs_) {
          if (usbUpdate_) {
            BootloaderEntry::enterUsbUpdate();
          } else {
            BootloaderEntry::enterOtaUpdate();
          }
        }
        break;
      }
      if (updateRequested_ && readyForUpdate()) {
        updateRequested_ = false;
        DebugLog::event(usbUpdate_ ? "entering USB bootloader"
                                   : "entering OTA bootloader");
        ble_.disconnect();
        updateResetAtMs_ = nowMs + kUpdateDisconnectMs;
        break;
      }
      if (redrawRequested_ && !hasReading_ && !pairingShown_) {
        redrawRequested_ = false;  // the first measurement draws anyway
      }
      if (redrawRequested_) {
        // Also commits a pending config, so a screen change costs one rail
        // cycle, not two.
        startCycle(nowMs, CycleKind::kRedraw);
      } else if (configLoaded_ && settingsSavePending() &&
                 nowMs >= nextConfigRetryMs_) {
        startCycle(nowMs, CycleKind::kCommitConfig);
      } else if (dumpRequested_) {
        dumpRequested_ = false;
        startCycle(nowMs, CycleKind::kDump);
      } else if (traceDumpRequested_) {
        traceDumpRequested_ = false;
        startCycle(nowMs, CycleKind::kTraceDump);
      } else if (stressRequested_) {
        stressRequested_ = false;
        startCycle(nowMs, CycleKind::kStress);
      } else if (sampleRequested_ || syncPending_ || frcPending_) {
        // A log sync or FRC rides a measurement cycle; start one now rather
        // than at the next deadline, whenever the request arrived.
        sampleRequested_ = false;
        startCycle(nowMs, CycleKind::kMeasure);
      } else if (scheduler_.claimDue(nowMs) ||
          pollCharging(nowMs) != renderedCharging_) {
        startCycle(nowMs, CycleKind::kMeasure);
      }
      break;

    case State::kPowering:
      if (power_.ready(nowMs)) {
        switch (cycleKind_) {
          case CycleKind::kMeasure:
            state_ = State::kInitializing;
            break;
          case CycleKind::kCommitConfig:
            state_ = State::kCommittingConfig;
            break;
          case CycleKind::kRedraw:
            state_ = State::kRedrawing;
            break;
          case CycleKind::kDump:
            state_ = State::kDumping;
            break;
          case CycleKind::kStress:
            state_ = State::kStressWriting;
            break;
          case CycleKind::kTraceDump:
            state_ = State::kTraceDumping;
            break;
        }
      }
      break;

    case State::kDumping:
      serviceDump();
      break;

    case State::kStressWriting:
      serviceStress(nowMs);
      break;

    case State::kTraceDumping:
      // One step: at most 48 KB over USB, only on request from the console.
      if (flash_.begin()) {
        TraceStore::dump(flash_);
      }
      state_ = State::kShuttingDown;
      break;

    case State::kRedrawing:
      if (settingsSavePending() && nowMs >= nextConfigRetryMs_) {
        commitConfig(nowMs);
      }
      redrawRequested_ = false;
      reading_ = rawReading_;
      applyCalibration(reading_, config_);
      state_ = State::kRendering;
      break;

    case State::kInitializing:
      environmentalSampler_.begin();
      if (environmentalSampler_.scd41Serial() != publishedScd41Serial_) {
        publishDeviceInfo();
      }
      if (environmentalSampler_.scd41AscDisabled() != publishedAscDisabled_) {
        publishedAscDisabled_ = environmentalSampler_.scd41AscDisabled();
        ble_.publishAscDisabled(publishedAscDisabled_);
      }
      microphone_.begin();
      if (!configLoaded_) loadSettings(nowMs);
      state_ = State::kAcquiring;
      break;

    case State::kAcquiring:
      reading_ = Reading{};
      reading_.monotonicMs = nowMs;
      reading_.epochMs = wallClock_.epochMsAt(nowMs);
      environmentalSampler_.start(nowMs);
      microphone_.start(nowMs);
      state_ = State::kWaiting;
      break;

    case State::kWaiting:
      environmentalSampler_.poll(nowMs);
      microphone_.poll(nowMs);
      if (environmentalSampler_.ready() && microphone_.ready()) {
        state_ = State::kCollecting;
      }
      break;

    case State::kCollecting: {
      const bool present = environmentalSampler_.bme280Present();
      const bool timedOut = environmentalSampler_.bme280TimedOut();
      const bool lightPresent = environmentalSampler_.veml7700Present();
      const bool lightTimedOut = environmentalSampler_.veml7700TimedOut();
      environmentalSampler_.collect(reading_);
      constexpr uint32_t kBme280Readings =
          VALID_TEMPERATURE | VALID_HUMIDITY | VALID_PRESSURE;
      const bool succeeded =
          (reading_.valid & kBme280Readings) == kBme280Readings;
      recordDeviceResult(faults_.bme280, present, timedOut, succeeded);
      recordDeviceResult(faults_.veml7700, lightPresent, lightTimedOut,
                         reading_.valid & VALID_LIGHT);
      recordDeviceResult(faults_.scd41, environmentalSampler_.scd41Present(),
                         environmentalSampler_.scd41TimedOut(),
                         reading_.valid & VALID_CO2);

      microphone_.collect(reading_);
      recordDeviceResult(faults_.microphone, microphone_.present(),
                         microphone_.timedOut(), reading_.valid & VALID_NOISE);

      // Without a cell the divider reads the charger's output, not a battery;
      // leave the battery monitor absent and the voltage invalid.
      if (kBatteryFitted) {
        float volts = 0.0f;
        const bool batteryOk = batteryMonitor_.sample(volts);
        if (batteryOk) {
          reading_.batteryV = volts;
          reading_.valid |= VALID_BATTERY;
        }
        recordDeviceResult(faults_.battery, true, !batteryOk, batteryOk);
      }
      rawReading_ = reading_;
      hasReading_ = true;
      applyCalibration(reading_, config_);
      // This cycle draws with the offsets and screen now in force; a change
      // after this point queues its own redraw.
      redrawRequested_ = false;
      if (frcPending_ && environmentalSampler_.scd41Present()) {
        frcState_ = BleCodec::kFrcSoaking;
        frcDeadlineMs_ = nowMs + kFrcSoakMs;
        environmentalSampler_.scd41StartFrcRunUp(nowMs);
        publishStatus();
        state_ = State::kFrcSoaking;
      } else {
        if (frcPending_) {  // requested but the sensor is absent this cycle
          frcPending_ = false;
          frcState_ = BleCodec::kFrcFailed;
        }
        state_ = State::kPersisting;
      }
      break;
    }

    case State::kFrcSoaking: {
      if (!frcPending_) {
        state_ = State::kPersisting;
        break;
      }
      const auto progress = environmentalSampler_.scd41PollFrcRunUp(nowMs);
      if (progress == Scd41Sensor::FrcProgress::kFailed ||
          nowMs >= frcDeadlineMs_) {
        frcPending_ = false;
        frcState_ = BleCodec::kFrcFailed;
        frcCorrectionPpm_ = 0;
        publishStatus();
        state_ = State::kPersisting;
      } else if (progress == Scd41Sensor::FrcProgress::kReady) {
        state_ = State::kFrcExecuting;
      }
      break;
    }

    case State::kFrcExecuting: {
      int16_t correction = 0;
      const bool ok = environmentalSampler_.scd41ForcedRecalibration(
                          frcTargetPpm_, correction);
      frcState_ = ok ? BleCodec::kFrcDone : BleCodec::kFrcFailed;
      frcCorrectionPpm_ = ok ? correction : 0;
      frcPending_ = false;
      if (ok) {  // saved by kPersisting, next
        config_.frcEpochSeconds =
            static_cast<uint32_t>(wallClock_.epochMsAt(nowMs) / 1000);
        config_.frcTargetPpm = frcTargetPpm_;
        config_.frcCorrectionPpm = correction;
        configSavePending_ = true;
        ble_.publishConfig(config_);
      }
      state_ = State::kPersisting;
      break;
    }

    case State::kPersisting: {
      // begin() here rather than in kInitializing: the flash spends the
      // sensor wait in deep power-down instead of idling for ~6 s.
      const bool flashPresent = flash_.begin();
      if (!configLoaded_) {
        // Unknown config/erase intent: do not mutate storage or expose history.
        recordDeviceResult(faults_.flash, flashPresent, flash_.timedOut(), false);
        state_ = State::kRendering;
        break;
      }
      persistConfigSaved_ =
          flashPresent ? saveSettings(nowMs) : !settingsSavePending();
      if (logErasePending_ && logEraseSequence_ == 0) {
        recordDeviceResult(faults_.flash, flashPresent, flash_.timedOut(), false);
        state_ = State::kRendering;
        break;
      }
      if (flashPresent && logErasePending_ && !logEraseCompleted_) {
        // This cycle's reading becomes the first record of the fresh log.
        logErasePending_ = false;
        sampleLog_.beginErase(logEraseSequence_);
        state_ = State::kErasingLog;
        publishStatus();
        break;
      }
      finishPersisting(flashPresent);
      break;
    }

    case State::kErasingLog: {
      // One sector per step (~45 ms), so BLE and the watchdog keep running
      // through the ~90 s erase.
      const SampleLog::EraseProgress progress = sampleLog_.eraseStep(flash_);
      if (progress == SampleLog::EraseProgress::kRunning) {
        break;
      }
      const bool erased = progress == SampleLog::EraseProgress::kDone;
      logErasePending_ = true;  // cleared only after a fresh record is durable
      logEraseCompleted_ = erased;
      finishPersisting(erased);
      publishStatus();
      break;
    }

    case State::kRendering: {
      const bool charging = pollCharging(nowMs);
      renderedCharging_ = charging;
      const UiModel model =
          pairingShown_ ? buildPairingModel(pairingCode_)
                        : buildUiModel(reading_, config_.displayScreen,
                                       config_.temperatureUnit, charging,
                                       judgeModeAt(nowMs));
      // The panel keeps its image unpowered, so an unchanged model means no
      // init and no refresh at all this cycle.
      const bool contentChanged =
          !hasRendered_ || !hasSameRenderedContent(model, lastRendered_);
      bool draw = contentChanged;
      bool fullRefresh =
          !hasRendered_ || config_.fullRefreshEveryCycles == 1 ||
          partialRefreshesSinceFull_ >=
              config_.fullRefreshEveryCycles - 1;
      bool displaySucceeded = false;
      if (draw) {
        const bool began = display_.begin(fullRefresh);
        const bool rendered = began && Renderer::render(display_, model);
        const bool ended = display_.end();
        displaySucceeded = began && rendered && ended;
        recordDeviceResult(faults_.display, display_.present(),
                           display_.timedOut(),
                           displaySucceeded);
        if (displaySucceeded) {
          lastRendered_ = model;
          hasRendered_ = true;
          partialRefreshesSinceFull_ = fullRefresh
                                           ? 0
                                           : partialRefreshesSinceFull_ + 1;
        }
      }
      DebugLog::cycleComplete(cycleNumber_, reading_, faults_, model.screen,
                              charging, draw && displaySucceeded,
                              sampleLog_.lastSequence());
      ble_.publishReading(reading_);
      publishDiagnostics(nowMs);
      if (cycleKind_ != CycleKind::kMeasure) {
        state_ = State::kShuttingDown;  // a pending sync waits for a measurement
      } else if (syncPending_ && ble_.connected() && configLoaded_ &&
                 !logErasePending_ && !sampleLog_.erasing()) {
        // The flash is still begun from kPersisting; stream before shutdown.
        beginSyncSession(nowMs);
        state_ = syncActive_ ? State::kSyncing : State::kShuttingDown;
      } else {
        syncPending_ = false;  // requester is gone; drop the stale request
        state_ = State::kShuttingDown;
      }
      publishStatus();
      break;
    }

    case State::kSyncing:
      serviceSync(nowMs);
      break;

    case State::kShuttingDown:
      // Trace builds: this cycle's lines go to the flash ring while the flash
      // is still begun from kPersisting.
      if (cycleKind_ == CycleKind::kMeasure && flash_.present()) {
        TraceStore::flush(flash_);
      }
      microphone_.end();
      flash_.end();  // deep power-down before the rail drops
      environmentalSampler_.end();
      power_.disable();
      state_ = State::kIdle;
      break;

    case State::kCommittingConfig:
      commitConfig(nowMs);
      power_.disable();
      state_ = State::kIdle;
      break;
  }

  // PDM event handoff needs millisecond service only during an active cycle.
  // Idle polling is bounded by BLE responsiveness and charger debounce rather
  // than waking the application thread at 1 kHz for minutes at a time.
  // Debug builds flag steps that starve BLE.poll(); the state is the enum
  // index (App.h), the value is how long the step took.
  const uint64_t stepMs = clock_.nowMs() - nowMs;
  if (stepMs > 150) {
    DebugLog::event("step slow ms", static_cast<long>(stepMs));
    DebugLog::event("step slow state", static_cast<long>(stepState));
  }

  if (state_ == State::kIdle) {
    delay(ble_.active() ? 20 : 50);
  } else {
    delay(1);
  }
}

void App::startCycle(uint64_t nowMs, CycleKind kind) {
  if (kind == CycleKind::kMeasure) {
    ++cycleNumber_;
  }
  cycleKind_ = kind;
  // A redraw or config commit reads no sensor, so it needs only the bus
  // devices' short settle: a screen change lands ~1 s sooner.
  const bool busOnly =
      kind == CycleKind::kRedraw || kind == CycleKind::kCommitConfig;
  power_.enable(nowMs, busOnly ? PowerDomain::kBusSettleMs
                               : PowerDomain::kSettleMs);
  state_ = State::kPowering;
}

bool App::loadSettings(uint64_t nowMs) {
  const bool flashPresent = flash_.begin();
  Config stored;
  BondTable bonds;
  SleepWindow window;
  uint32_t eraseSequence = 0;
  const bool loaded = flashPresent && configStore_.load(flash_, stored);
  bool ok = flashPresent && configStore_.healthy();
  if (ok) {
    configStore_.loadBonds(flash_, bonds);
    ok = configStore_.healthy();
  }
  if (ok) {
    // None stored (a unit older than the window) is the default, not a fault.
    configStore_.loadSleepWindow(flash_, window);
    ok = configStore_.healthy() && configStore_.loadLogErase(flash_, eraseSequence);
  }
  if (ok) {
    if (loaded) config_ = stored;
    configSavePending_ = !loaded;
    nextConfigRetryMs_ = nowMs;
    bonds_ = bonds;
    ble_.setBonds(bonds_);
    sleepWindow_ = window;
    ble_.publishSleepWindow(sleepWindow_);
    logEraseSequence_ = eraseSequence;
    logErasePending_ = eraseSequence != 0;
    // Boot counter failure must not overwrite an unknown counter on retry.
    ok = configStore_.advanceBootCount(flash_, bootCount_);
  }
  recordDeviceResult(faults_.flash, flashPresent, flash_.timedOut(), ok);
  flash_.end();
  if (!ok) return false;
  sampleLog_.setBootCount(bootCount_);
  scheduler_.changeInterval(nowMs, config_.measurementIntervalSeconds);
  configLoaded_ = true;
  publishDeviceInfo();
  const bool bleReady = ble_.begin(config_, serialNumber_, nowMs);
  recordDeviceResult(faults_.ble, bleReady, false, bleReady);
  return true;
}

void App::commitConfig(uint64_t nowMs) {
  const bool flashPresent = flash_.begin();
  const bool saved = flashPresent && saveSettings(nowMs);
  recordDeviceResult(faults_.flash, flashPresent, flash_.timedOut(), saved);
  flash_.end();
  if (!flashPresent) {
    nextConfigRetryMs_ = nowMs + 60000;
  }
}

// Writes whatever config and bonds are pending; the flash must be begun.
// A failure retries in 60 s.
bool App::saveSettings(uint64_t nowMs) {
  // An erase command that arrived before the log was mounted is checked now,
  // before anything is journalled: the newest record must not be later than
  // the app's up-to sequence, or nothing is erased (PROTOCOL.md §6.11).
  if (logEraseRequested_) {
    if (!sampleLog_.mount(flash_)) {
      nextConfigRetryMs_ = nowMs + 60000;
      return false;
    }
    logEraseRequested_ = false;
    if (sampleLog_.lastSequence() <= logEraseUpTo_) {
      startLogErase();
    } else {
      DebugLog::event("log erase refused, newest",
                      static_cast<long>(sampleLog_.lastSequence()));
    }
  }
  // Journal deletion before any reset settings/bonds are committed, including
  // an idle config-only cycle. Never erase or append without durable intent.
  if (logErasePending_ && logEraseSequence_ == 0) {
    if (!sampleLog_.mount(flash_) ||
        !configStore_.saveLogErase(flash_, sampleLog_.lastSequence() + 1)) {
      nextConfigRetryMs_ = nowMs + 60000;
      return false;
    }
    logEraseSequence_ = sampleLog_.lastSequence() + 1;
  }
  if (configSavePending_ && configStore_.save(flash_, config_)) {
    configSavePending_ = false;
  }
  if (bondsSavePending_ && configStore_.saveBonds(flash_, bonds_)) {
    bondsSavePending_ = false;
  }
  if (sleepSavePending_ &&
      configStore_.saveSleepWindow(flash_, sleepWindow_)) {
    sleepSavePending_ = false;
  }
  const bool saved = !settingsSavePending();
  if (!saved) {
    nextConfigRetryMs_ = nowMs + 60000;
  }
  return saved;
}

void App::servicePairing(uint64_t nowMs) {
  const bool usbPowered = batteryMonitor_.usbPowered();
  ble_.setPairingAllowed(usbPowered);
  if (ble_.takeBondsChanged(bonds_)) {
    bondsSavePending_ = true;
    nextConfigRetryMs_ = nowMs;
  }
  const bool bonded = bonds_.count() > 0;
  if (usbPowered != publishedPairingOpen_ || bonded != publishedBonded_) {
    publishDeviceInfo();
  }

  if (ble_.takeEnrollment(pairingCode_)) {
    pairingShown_ = true;
    pairingUntilMs_ = nowMs + kPairingScreenMs;
    redrawRequested_ = true;
  } else if (pairingShown_ &&
             (nowMs >= pairingUntilMs_ || !ble_.connected() ||
              !ble_.enrollmentPending() || ble_.authenticated())) {
    pairingShown_ = false;  // paired, abandoned or timed out
    pairingCode_ = 0;
    redrawRequested_ = true;
  }
}

void App::finishPersisting(bool flashPresent) {
  bool logged = flashPresent && sampleLog_.append(flash_, reading_);
  if (logged && logEraseCompleted_) {
    if (configStore_.saveLogErase(flash_, 0)) {
      logErasePending_ = false;
      logEraseSequence_ = 0;
      logEraseCompleted_ = false;
    } else {
      logged = false;
    }
  }
  recordDeviceResult(faults_.flash, flashPresent, flash_.timedOut(),
                     logged && persistConfigSaved_);
  state_ = State::kRendering;
}

void App::takeBleCommands(uint64_t nowMs) {
  BleCodec::DeviceControlRequest control;
  if (ble_.takePendingDeviceControl(control)) {
    if (control.opcode == BleCodec::kControlFactoryReset) {
      factoryReset(nowMs, control.eraseLog);
    } else if (control.opcode == BleCodec::kControlEnterUpdate) {
      if (!kBleOtaEnabled) {
        DebugLog::event("update ignored (BLE OTA disabled)");
      } else if (updateAllowed()) {
        updateRequested_ = true;
        usbUpdate_ = false;
      } else {
        DebugLog::event("update refused (low battery)");
      }
    } else if (control.opcode == BleCodec::kControlEnterUsbUpdate) {
      // The drive only appears on USB; off USB the bootloader would just
      // start the firmware again, so refuse rather than reboot for nothing.
      if (batteryMonitor_.usbPowered()) {
        updateRequested_ = true;
        usbUpdate_ = true;
      } else {
        DebugLog::event("USB update refused (not on USB)");
      }
    } else {
      requestLogErase(nowMs, control.upToSequence);
    }
  }
  applySleepWindow(nowMs);
  uint16_t frcTarget = 0;
  if (ble_.takePendingFrcRequest(frcTarget) && !frcPending_) {
    frcPending_ = true;
    frcTargetPpm_ = frcTarget;
    frcState_ = BleCodec::kFrcPending;
    frcCorrectionPpm_ = 0;
  }
  BleCodec::LogSyncRequest request;
  if (ble_.takePendingSyncRequest(request)) {
    if (request.opcode == BleCodec::kSyncOpAbort) {
      syncAbort_ = true;
      syncPending_ = false;
    } else if (!syncActive_ && !syncPending_) {
      syncRequest_ = request;
      syncPending_ = true;
    }
  }
}

void App::beginSyncSession(uint64_t nowMs) {
  syncPending_ = false;
  syncAbort_ = false;
  syncAttPayload_ = BleCodec::clampAttPayload(syncRequest_.attPayload);
  syncPacketSeq_ = 0;
  syncSentRecords_ = 0;
  syncFragIndex_ = 0;
  syncBatchCount_ = 0;
  syncEndPending_ = false;
  lastSentSequence_ =
      syncRequest_.startSequence > 0 ? syncRequest_.startSequence - 1 : 0;
  syncDeadlineMs_ = nowMs + kSyncProgressTimeoutMs;
  syncActive_ =
      sampleLog_.startRead(flash_, syncRequest_.startSequence, syncCursor_);
}

void App::serviceSync(uint64_t nowMs) {
  if (syncAbort_ || !ble_.connected() || nowMs >= syncDeadlineMs_ ||
      !syncCursor_.active) {
    finishSync();
    return;
  }
  // One notification per pass, at most one per kSyncPacketGapMs; the cursor
  // only advances after the stack took the packet, so a refusal retries the
  // same packet on the next pass.
  if (nowMs - syncLastSendMs_ < kSyncPacketGapMs) {
    return;
  }
  syncLastSendMs_ = nowMs;
  {
    if (!syncEndPending_ && syncBatchCount_ == 0) {
      uint8_t wanted = BleCodec::recordsPerPacket(syncAttPayload_);
      if (wanted == 0) {
        wanted = 1;  // fragment path carries one record at a time
      }
      if (wanted > kSyncBatchMax) {
        wanted = kSyncBatchMax;
      }
      if (syncRequest_.maxRecords != 0) {
        const uint16_t left = syncRequest_.maxRecords - syncSentRecords_;
        if (wanted > left) {
          wanted = static_cast<uint8_t>(left);
        }
      }
      while (syncBatchCount_ < wanted &&
             sampleLog_.readNext(flash_, syncCursor_,
                                 syncBatch_[syncBatchCount_])) {
        syncBatchCount_++;
      }
      if (!syncCursor_.active) {  // flash failure mid-read
        finishSync();
        return;
      }
      if (syncBatchCount_ == 0) {
        syncEndPending_ = true;
      }
    }

    if (syncEndPending_) {
      const uint16_t length = BleCodec::buildLogEndPacket(
          syncPacketSeq_, lastSentSequence_ + 1, syncPacket_);
      if (!ble_.notifyLogData(syncPacket_, length)) {
        return;
      }
      finishSync();
      return;
    }

    if (BleCodec::recordsPerPacket(syncAttPayload_) == 0) {
      if (syncFragIndex_ == 0) {
        BleCodec::sampleRecordToWire(syncBatch_[0], syncWire_);
      }
      const uint16_t length = BleCodec::buildLogFragmentPacket(
          syncPacketSeq_, syncRemainingRecords(), syncWire_,
          syncBatch_[0].sequence, syncFragIndex_, syncAttPayload_,
          syncPacket_);
      if (!ble_.notifyLogData(syncPacket_, length)) {
        return;
      }
      syncPacketSeq_++;
      syncFragIndex_++;
      if (syncFragIndex_ >= BleCodec::fragmentsPerRecord(syncAttPayload_)) {
        lastSentSequence_ = syncBatch_[0].sequence;
        syncSentRecords_++;
        syncFragIndex_ = 0;
        syncBatchCount_ = 0;
      }
    } else {
      const uint16_t length = BleCodec::buildLogDataPacket(
          syncPacketSeq_, syncRemainingRecords(), syncBatch_, syncBatchCount_,
          syncPacket_);
      if (!ble_.notifyLogData(syncPacket_, length)) {
        return;
      }
      syncPacketSeq_++;
      lastSentSequence_ = syncBatch_[syncBatchCount_ - 1].sequence;
      syncSentRecords_ += syncBatchCount_;
      syncBatchCount_ = 0;
    }
    syncDeadlineMs_ = nowMs + kSyncProgressTimeoutMs;
  }
}

void App::finishSync() {
  syncActive_ = false;
  syncAbort_ = false;
  syncPending_ = false;
  syncEndPending_ = false;
  syncBatchCount_ = 0;
  syncFragIndex_ = 0;
  publishStatus();
  state_ = State::kShuttingDown;
}

uint16_t App::syncRemainingRecords() const {
  const uint32_t last = sampleLog_.lastSequence();
  if (last <= lastSentSequence_) {
    return 0;
  }
  const uint32_t remaining = last - lastSentSequence_;
  return remaining > UINT16_MAX ? UINT16_MAX
                                : static_cast<uint16_t>(remaining);
}

void App::factoryReset(uint64_t nowMs, bool eraseLog) {
  // The FRC correction stays in the sensor, so its record stays too.
  Config defaults = UnitIdentity::defaultConfigForUnit(deviceId_);
  defaults.frcEpochSeconds = config_.frcEpochSeconds;
  defaults.frcTargetPpm = config_.frcTargetPpm;
  defaults.frcCorrectionPpm = config_.frcCorrectionPpm;
  config_ = defaults;
  scheduler_.changeInterval(nowMs, config_.measurementIntervalSeconds);
  ble_.publishConfig(config_);
  ble_.applyAvailabilityPolicy(config_, nowMs);
  configSavePending_ = true;
  nextConfigRetryMs_ = nowMs;
  // Forget every phone, the requester included; its link stays authenticated
  // until it disconnects. Pairing again needs USB power.
  ble_.clearBonds();
  frcPending_ = false;  // also ends a running soak (see kFrcSoaking)
  frcState_ = BleCodec::kFrcIdle;
  frcCorrectionPpm_ = 0;
  sleepWindow_ = defaultSleepWindow();
  ble_.publishSleepWindow(sleepWindow_);
  sleepSavePending_ = true;
  if (eraseLog) {
    logEraseRequested_ = false;  // the reset erases whatever the app asked
    startLogErase();
  }
  // Unconditional, unlike a config write: a reset that lands mid-cycle still
  // gets its own cycle to redraw the defaults and run the erase.
  sampleRequested_ = true;
  publishStatus();
}

// An update erases the running image before it writes the new one, so it
// must not start on a battery that could die halfway (PROTOCOL.md §9.2).
bool App::updateAllowed() const {
  if (batteryMonitor_.usbPowered()) {
    return true;
  }
  return (reading_.valid & VALID_BATTERY) != 0 &&
         reading_.batteryV >= kUpdateMinBatteryVolts;
}

// Nothing in RAM that the reset would lose: settings saved, no erase or FRC
// still to run. Until then the idle branches below do that work first.
bool App::readyForUpdate() const {
  return !settingsSavePending() && !logErasePending_ && !logEraseRequested_ &&
         !sampleLog_.erasing() && !frcPending_ && !syncPending_;
}

// The erase command (PROTOCOL.md §6.11): the factory reset's erase without
// the reset. Refused, with nothing changed, when the log holds a record later
// than upToSequence, so the app cannot erase what it has not downloaded.
void App::requestLogErase(uint64_t nowMs, uint32_t upToSequence) {
  if (logErasePending_ || logEraseRequested_ || sampleLog_.erasing() ||
      state_ == State::kErasingLog) {
    DebugLog::event("log erase ignored (already erasing)");
    return;
  }
  if (sampleLog_.mounted()) {
    if (sampleLog_.lastSequence() > upToSequence) {
      DebugLog::event("log erase refused, newest",
                      static_cast<long>(sampleLog_.lastSequence()));
      return;
    }
    startLogErase();
  } else {
    // Only before the first record of this boot: saveSettings() decides.
    logEraseRequested_ = true;
    logEraseUpTo_ = upToSequence;
  }
  nextConfigRetryMs_ = nowMs;
  sampleRequested_ = true;  // the erase rides a measurement, as on reset
  publishStatus();
}

// Same journalled, power-loss-safe path for both: saveSettings() records the
// intent and the sequence floor, kPersisting erases.
void App::startLogErase() {
  logErasePending_ = true;
  syncAbort_ = true;  // any running download ends
  syncPending_ = false;
}

void App::applySleepWindow(uint64_t nowMs) {
  SleepWindow window;
  if (!ble_.takePendingSleepWindow(window)) {
    return;
  }
  uint8_t before[SleepWindow::kBytes];
  uint8_t after[SleepWindow::kBytes];
  encodeSleepWindow(sleepWindow_, before);
  encodeSleepWindow(window, after);
  ble_.publishSleepWindow(window);
  if (memcmp(before, after, sizeof before) == 0) {
    return;  // the app rewrites the offset on every connect
  }
  const JudgeMode modeBefore = judgeModeAt(nowMs);
  sleepWindow_ = window;
  sleepSavePending_ = true;
  nextConfigRetryMs_ = nowMs;
  // Redraw only when the verdicts change now; otherwise the next
  // measurement picks the window up.
  redrawRequested_ = redrawRequested_ || judgeModeAt(nowMs) != modeBefore;
}

JudgeMode App::judgeModeAt(uint64_t nowMs) const {
  return SleepSchedule::panelJudgeMode(sleepWindow_, wallClock_.synced(),
                                       wallClock_.epochMsAt(nowMs));
}

// Generous backstops, not timing: each state's own deadline normally ends it
// long before. Past its bound a state is stuck, and HardwareWatchdog resets
// the unit kTimeoutMs later.
uint32_t App::stateLimitMs(State state) {
  switch (state) {
    case State::kFrcSoaking:
      return kFrcSoakMs + 60000;
    case State::kErasingLog:
      return 20UL * 60 * 1000;  // ~2000 sectors at up to 500 ms each
    case State::kSyncing:
      return 60UL * 60 * 1000;  // a full log at the minimum MTU
    case State::kDumping:
      return 30UL * 60 * 1000;  // a full log over USB serial
    default:
      return 60000;
  }
}

// Debug builds only: pollCommand() is always 0 in a release build.
void App::handleConsole(uint64_t nowMs) {
  switch (DebugLog::pollCommand()) {
    case 'i': {
      DebugLog::Info info;
      info.firmwareVersion = FirmwareVersion::kString;
      info.serialNumber = serialNumber_;
      info.resetReason = ResetReason::describe(resetReason_);
      info.bootCount = bootCount_;
      info.lastSequence = sampleLog_.lastSequence();
      info.bondCount = ble_.bondCount();
      info.usbPowered = batteryMonitor_.usbPowered();
      info.uptimeMs = nowMs;
      DebugLog::info(info, config_);
      break;
    }
    case 'm':
      sampleRequested_ = true;
      break;
    case 'd':
      dumpRequested_ = true;  // starts from idle
      break;
    case 't':
      traceDumpRequested_ = true;  // starts from idle; trace builds only
      break;
    case 'w':
      stressRequested_ = true;
      break;
    case '?':
    case 'h':
      DebugLog::help();
      break;
    default:
      break;
  }
}

// Streams the whole log as CSV, a bounded batch per step so BLE and the
// watchdog keep running.
void App::serviceDump() {
  if (logErasePending_ || sampleLog_.erasing()) {
    DebugLog::logDumpEnd(0, false);
    dumpStarted_ = false;
    state_ = State::kShuttingDown;
    return;
  }
  if (!dumpStarted_) {
    dumpStarted_ = true;
    dumpCount_ = 0;
    const bool flashPresent = flash_.begin();
    DebugLog::logDumpBegin(serialNumber_, sampleLog_.lastSequence());
    if (!flashPresent || !sampleLog_.startRead(flash_, 0, dumpCursor_)) {
      DebugLog::logDumpEnd(0, false);
      dumpStarted_ = false;
      state_ = State::kShuttingDown;
    }
    return;
  }
  for (uint8_t i = 0; i < kDumpRecordsPerStep; i++) {
    SampleRecord record;
    if (!sampleLog_.readNext(flash_, dumpCursor_, record)) {
      DebugLog::logDumpEnd(dumpCount_, dumpCursor_.active);
      dumpStarted_ = false;
      state_ = State::kShuttingDown;
      return;
    }
    DebugLog::logDumpRecord(record);
    dumpCount_++;
  }
}

// Each step is one flash write: a config save or a log append of an empty
// record (no valid readings), so a power pull hits a write almost surely.
// After power returns, the boot must find the old or new config and a log
// whose only damage is at most the record being written.
void App::serviceStress(uint64_t nowMs) {
  if (logErasePending_ || sampleLog_.erasing()) {
    stressStarted_ = false;
    state_ = State::kShuttingDown;
    return;
  }
  if (!stressStarted_) {
    stressStarted_ = true;
    stressOps_ = 0;
    stressFailures_ = 0;
    stressEndMs_ = nowMs + kStressMs;
    if (!flash_.begin()) {
      DebugLog::stress(0, 1, sampleLog_.lastSequence(), true);
      stressStarted_ = false;
      state_ = State::kShuttingDown;
      return;
    }
    DebugLog::stress(0, 0, sampleLog_.lastSequence(), false);
    return;
  }
  bool ok;
  if (stressOps_ % 2 == 0) {
    ok = configStore_.save(flash_, config_);
  } else {
    Reading filler;
    filler.monotonicMs = nowMs;
    filler.epochMs = wallClock_.epochMsAt(nowMs);
    ok = sampleLog_.append(flash_, filler);
  }
  stressOps_++;
  stressFailures_ += ok ? 0 : 1;
  const bool done = nowMs >= stressEndMs_;
  if (done || stressOps_ % 50 == 0) {
    DebugLog::stress(stressOps_, stressFailures_, sampleLog_.lastSequence(),
                     done);
  }
  if (done) {
    stressStarted_ = false;
    state_ = State::kShuttingDown;
  }
}

void App::publishDiagnostics(uint64_t nowMs) {
  BleCodec::Diagnostics diagnostics;
  diagnostics.resetReason = resetReason_;
  diagnostics.uptimeSeconds = static_cast<uint32_t>(nowMs / 1000);
  diagnostics.i2cBusStuck = !environmentalSampler_.busFree();
  ble_.publishDiagnostics(diagnostics);
}

void App::publishDeviceInfo() {
  publishedScd41Serial_ = environmentalSampler_.scd41Serial();
  BleCodec::DeviceInfo info;
  info.firmwareMajor = FirmwareVersion::kMajor;
  info.firmwareMinor = FirmwareVersion::kMinor;
  info.firmwarePatch = FirmwareVersion::kPatch;
  info.debugBuild = kDebugEnabled;
  info.scd41Serial = publishedScd41Serial_;
  info.bootCount = bootCount_;
  publishedPairingOpen_ = batteryMonitor_.usbPowered();
  publishedBonded_ = bonds_.count() > 0;
  info.pairingOpen = publishedPairingOpen_;
  info.bonded = publishedBonded_;
  info.noBattery = !kBatteryFitted;
  ble_.publishDeviceInfo(info);
}

void App::publishStatus() {
  BleCodec::StatusInfo info;
  info.faults = &faults_;
  info.validFlags = reading_.valid;
  info.lastSequence = sampleLog_.lastSequence();
  info.timeSynced = wallClock_.synced();
  info.syncActive = syncActive_;
  info.logErasing = logErasePending_ || state_ == State::kErasingLog;
  info.frcState = frcState_;
  info.frcCorrectionPpm = frcCorrectionPpm_;
  info.charging = renderedCharging_;
  ble_.publishStatus(info);
}

bool App::pollCharging(uint64_t nowMs) {
  return kBatteryFitted && batteryMonitor_.pollCharging(nowMs);
}

void App::applyBleConfig(uint64_t nowMs) {
  Config candidate;
  if (!ble_.takePendingConfig(candidate)) {
    return;
  }
  const bool displayChanged =
      candidate.displayScreen != config_.displayScreen ||
      candidate.temperatureUnit != config_.temperatureUnit;
  const bool calibrationChanged =
      candidate.noiseOffsetDb != config_.noiseOffsetDb ||
      candidate.tempOffsetC != config_.tempOffsetC ||
      candidate.humidityOffsetRh != config_.humidityOffsetRh;
  const bool intervalChanged = candidate.measurementIntervalSeconds !=
                               config_.measurementIntervalSeconds;
  config_ = candidate;
  if (intervalChanged) {
    scheduler_.changeInterval(nowMs, config_.measurementIntervalSeconds);
  }
  ble_.applyAvailabilityPolicy(config_, nowMs);
  configSavePending_ = true;
  nextConfigRetryMs_ = nowMs;
  // Redraw from the last reading rather than wait ~7 s for a measurement;
  // mid-cycle, the flag survives until idle unless the cycle has yet to
  // collect (see kCollecting).
  redrawRequested_ = redrawRequested_ || displayChanged || calibrationChanged;
}
