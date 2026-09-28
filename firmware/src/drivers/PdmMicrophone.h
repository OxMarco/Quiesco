// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

#include "../dsp/AcousticMetrics.h"

struct Reading;

// SPH0641LU4H-1 via the nRF52840 PDM peripheral, driven directly with the
// register sequence proven by 04_test_mic: mono, falling edge (SELECT is
// strapped low on v1), continuous clock with EasyDMA double-buffering.
// Completed buffers are consumed by poll() from the main context -- at one
// poll per ~1 ms against 63.5 ms per buffer no interrupt is needed, which
// keeps every rule in SOFTWARE.md §3 trivially satisfied.
class PdmMicrophone {
 public:
  static constexpr uint16_t kSamplesPerBuffer = 1024;  // 63.5 ms at ~16.1 kHz
  // The mic sleeps whenever its clock stops; after start it needs its wake
  // time plus the nRF decimation-filter settle before buffers are audio.
  static constexpr uint8_t kDiscardBuffers = 8;   // ~0.5 s
  // ~7.1 s window: the cycle waits ~10 s for the SCD41 anyway, so a long
  // Leq costs only the mic's own current, and one door slam no longer
  // dominates the reading as it could in a 1 s snapshot.
  static constexpr uint8_t kCaptureBuffers = 112;
  static constexpr uint32_t kCaptureTimeoutMs = 10000;
  static constexpr float kGainDb = 8.0f;  // GAINL/R = 0x38 (0.5 dB steps)

  bool begin();
  void start(uint64_t nowMs);
  void poll(uint64_t nowMs);
  bool ready() const;
  void collect(Reading& reading);
  void end();

  bool present() const;
  bool timedOut() const;

  // Raw capture statistics. A dB SPL number alone cannot be checked against
  // anything; rms/peak/dc/clip are what HARDWARE.md's known-good table is
  // written in, and they separate real audio from the stopped-clock transient.
  const AcousticMetrics& metrics() const { return metrics_; }

 private:
  bool stopPeripheral();

  enum class State : uint8_t {
    kUnavailable,
    kIdle,
    kCapturing,
    kComplete,
    kTimedOut,
  };

  AcousticMetrics metrics_;
  uint64_t timeoutAtMs_ = 0;
  uint8_t discardRemaining_ = 0;
  bool running_ = false;
  State state_ = State::kUnavailable;
};
