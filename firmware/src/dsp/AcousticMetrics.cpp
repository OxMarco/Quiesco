// SPDX-License-Identifier: GPL-3.0-only
#include "AcousticMetrics.h"

#include <math.h>
#include <stdlib.h>

namespace {

constexpr float kSilentRms = 1.0f;
// More than 10% of a settled window pinned at the rail is a fault, not sound.
constexpr float kMaxClippedFraction = 0.1f;
constexpr float kFullScale = 32768.0f;
// SPH0641LU4H-1: -26 dBFS at 94 dB SPL, so 0 dBFS ~ 120 dB SPL before gain.
constexpr float kSensitivityOffsetDb = 120.0f;

// A-weighting at 16 125 Hz as three biquads {b0, b1, b2, a1, a2}, a0 = 1:
// the curve's two high-pass pole pairs (20.6 Hz twice; 107.7 Hz and
// 737.9 Hz) by bilinear transform, then a high shelf (5.3 kHz, -3 dB,
// Q 0.4) standing in for the 12.2 kHz poles, which lie above Nyquist. Gain
// normalised to 0 dB at 1 kHz. Matches the IEC 61672 curve within 0.15 dB
// from 20 Hz to 8 kHz (IEC class 1 allows far more), checked by the host
// tests; derivation in SOFTWARE.md, acoustic measurement.
constexpr float kAWeighting[3][5] = {
    {1.2468354484e+00f, -2.4936708967e+00f, 1.2468354484e+00f,
     -1.9840111668e+00f, 9.8407507754e-01f},
    {8.5635163765e-01f, -1.7127032753e+00f, 8.5635163765e-01f,
     -1.7075393205e+00f, 7.1786723009e-01f},
    {8.7485527366e-01f, 4.5898999403e-01f, -2.2668209463e-02f,
     3.7788571709e-01f, -6.6708658865e-02f},
};

float levelDb(float rms, float pdmGainDb) {
  if (rms < 0.5f) {
    rms = 0.5f;  // floor ~ -96 dBFS, keeps log10 finite
  }
  return 20.0f * log10f(rms / kFullScale) - pdmGainDb + kSensitivityOffsetDb;
}

}  // namespace

void AcousticMetrics::reset() {
  sumSquares_ = 0;
  sumMeans_ = 0;
  samples_ = 0;
  peak_ = 0;
  clipped_ = 0;
  buffers_ = 0;
  for (uint8_t i = 0; i < kSections; i++) {
    state1_[i] = 0.0f;
    state2_[i] = 0.0f;
  }
  weightedSumSquares_ = 0.0;
  weightedSamples_ = 0;
}

void AcousticMetrics::addBuffer(const int16_t* samples, uint16_t count) {
  if (count == 0) {
    return;
  }
  int64_t sum = 0;
  for (uint16_t i = 0; i < count; i++) {
    sum += samples[i];
    if (samples[i] >= kClipLevel || samples[i] <= -kClipLevel) {
      clipped_++;
    }
  }
  const int32_t mean = static_cast<int32_t>(sum / count);

  for (uint16_t i = 0; i < count; i++) {
    // peak needs int32: near-full-scale minus an opposite-sign DC mean does
    // not fit in an int16.
    const int32_t s = samples[i] - mean;
    sumSquares_ += static_cast<uint64_t>(static_cast<int64_t>(s) * s);
    const int32_t magnitude = s < 0 ? -s : s;
    if (magnitude > peak_) {
      peak_ = magnitude;
    }
  }

  // A-weighted path. The first buffer only settles the filter (its 20 Hz
  // poles need tens of ms to forget the mic's DC offset) and is not counted.
  float weightedSum = 0.0f;
  for (uint16_t i = 0; i < count; i++) {
    float x = static_cast<float>(samples[i]);
    for (uint8_t k = 0; k < kSections; k++) {
      const float* c = kAWeighting[k];
      const float y = c[0] * x + state1_[k];
      state1_[k] = c[1] * x - c[3] * y + state2_[k];
      state2_[k] = c[2] * x - c[4] * y;
      x = y;
    }
    weightedSum += x * x;
  }
  if (buffers_ > 0) {
    weightedSumSquares_ += weightedSum;
    weightedSamples_ += count;
  }

  sumMeans_ += mean;
  samples_ += count;
  buffers_++;
}

float AcousticMetrics::weightedRms() const {
  if (weightedSamples_ == 0) {
    return 0.0f;
  }
  return static_cast<float>(
      sqrt(weightedSumSquares_ / static_cast<double>(weightedSamples_)));
}

float AcousticMetrics::dbA(float pdmGainDb) const {
  return levelDb(weightedRms(), pdmGainDb);
}

float AcousticMetrics::rms() const {
  if (samples_ == 0) {
    return 0.0f;
  }
  // The division must not be integer: a quiet room whose mean square is below
  // 1 would truncate to 0, report RMS 0, and then fail valid() -- turning a
  // usable quiet reading into no reading at all. Double because the numerator
  // reaches ~1.8e13 over a full window, past float's exact-integer range.
  return static_cast<float>(
      sqrt(static_cast<double>(sumSquares_) / static_cast<double>(samples_)));
}

int32_t AcousticMetrics::dcMean() const {
  if (buffers_ == 0) {
    return 0;
  }
  return static_cast<int32_t>(sumMeans_ / buffers_);
}

bool AcousticMetrics::valid() const {
  if (samples_ == 0 || rms() <= kSilentRms) {
    return false;
  }
  return clipped_ <= kMaxClippedFraction * samples_;
}

float AcousticMetrics::dbSpl(float pdmGainDb) const {
  return levelDb(rms(), pdmGainDb);
}
