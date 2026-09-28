// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

// Acoustic statistics accumulated over a capture window. Pure C++ (no
// Arduino/Nordic headers) so the math and the validity policy stay
// host-testable.
//
// Two paths run over the same samples:
// - unweighted RMS around each buffer's DC mean: the raw statistics that
//   HARDWARE.md's known-good table and the validity policy are written in;
// - A-weighted energy (IEC 61672 curve, see kAWeighting) for the reported
//   level, dB(A) Leq over the window: the unit sound meters, phone apps and
//   comfort guidelines use. The filter's high-pass sections also remove DC.
class AcousticMetrics {
 public:
  // Samples at or beyond this magnitude count as pinned at the rail; a
  // buffer full of them is the stopped-clock wake-up transient described in
  // HARDWARE.md, not audio.
  static constexpr int16_t kClipLevel = 32000;
  // Sample rate the A-weighting filter is designed for: PDM clock 1.032 MHz
  // decimated by 64.
  static constexpr float kSampleRateHz = 16125.0f;

  void reset();
  void addBuffer(const int16_t* samples, uint16_t count);

  uint16_t buffers() const { return buffers_; }
  float rms() const;        // around the per-buffer DC means
  int32_t peak() const { return peak_; }
  int32_t dcMean() const;   // mean of per-buffer means
  uint32_t clipped() const { return clipped_; }

  // A working mic idles well above RMS 1.0 even in a silent room (known-good
  // range 20-120); at or below it there is no data at all. Heavy clipping is
  // the stopped-clock signature. Either way the window is not audio.
  bool valid() const;

  // Estimated dB SPL from dBFS, the configured PDM gain, and the SPH0641's
  // nominal -26 dBFS @ 94 dB SPL sensitivity. Uncalibrated against a
  // reference (open Phase 0 item).
  float dbSpl(float pdmGainDb) const;
  // A-weighted equivalent continuous level, dB(A), with the same sensitivity
  // constants; this is the reported noise reading.
  float dbA(float pdmGainDb) const;
  float weightedRms() const;

 private:
  static constexpr uint8_t kSections = 3;
  float state1_[kSections] = {};  // transposed direct form II
  float state2_[kSections] = {};
  double weightedSumSquares_ = 0.0;
  uint64_t weightedSamples_ = 0;
  uint64_t sumSquares_ = 0;
  int64_t sumMeans_ = 0;
  uint64_t samples_ = 0;
  int32_t peak_ = 0;
  uint32_t clipped_ = 0;
  uint16_t buffers_ = 0;
};
