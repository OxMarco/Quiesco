// SPDX-License-Identifier: GPL-3.0-only
#include "PdmMicrophone.h"

#include <Arduino.h>

#include "../board/BoardPins.h"
#include "../diagnostics/Trace.h"
#include "../model/Reading.h"

namespace {

// EasyDMA needs word-aligned buffers; two so the peripheral fills one while
// the finished one is analyzed (it stays untouched for a full buffer period
// after the swap).
int16_t bufferA[PdmMicrophone::kSamplesPerBuffer] __attribute__((aligned(4)));
int16_t bufferB[PdmMicrophone::kSamplesPerBuffer] __attribute__((aligned(4)));
bool fillingA = true;

// PDM events fire within microseconds; this bounds a hardware fault, not a
// normal handshake.
bool waitEvent(volatile uint32_t& event) {
  for (uint32_t i = 0; i < 100000; ++i) {
    if (event) {
      return true;
    }
  }
  return false;
}

NRF_GPIO_Type* portFor(uint32_t psel) {
  return (psel >> 5) ? NRF_P1 : NRF_P0;
}

// PSEL only routes the signal; the GPIO directions must be configured
// separately (skip this and CLK is never driven, so the mic never wakes).
void configurePins() {
  NRF_GPIO_Type* clk = portFor(BoardPins::kPdmClockPsel);
  NRF_GPIO_Type* din = portFor(BoardPins::kPdmDataPsel);

  clk->PIN_CNF[BoardPins::kPdmClockPsel & 31] =
      (GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos) |
      (GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos) |
      (GPIO_PIN_CNF_PULL_Disabled << GPIO_PIN_CNF_PULL_Pos) |
      (GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos) |
      (GPIO_PIN_CNF_SENSE_Disabled << GPIO_PIN_CNF_SENSE_Pos);
  clk->OUTCLR = 1UL << (BoardPins::kPdmClockPsel & 31);

  din->PIN_CNF[BoardPins::kPdmDataPsel & 31] =
      (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos) |
      (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos) |
      (GPIO_PIN_CNF_PULL_Disabled << GPIO_PIN_CNF_PULL_Pos) |
      (GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos) |
      (GPIO_PIN_CNF_SENSE_Disabled << GPIO_PIN_CNF_SENSE_Pos);
}

void parkPins() {
  // PIN_CNF reset value: input, disconnected -- leakage-safe once the rail
  // drops the mic.
  portFor(BoardPins::kPdmClockPsel)
      ->PIN_CNF[BoardPins::kPdmClockPsel & 31] = 0x00000002;
  portFor(BoardPins::kPdmDataPsel)
      ->PIN_CNF[BoardPins::kPdmDataPsel & 31] = 0x00000002;
}

void configurePeripheral() {
  NRF_PDM->ENABLE = 0;
  NRF_PDM->PSEL.CLK = BoardPins::kPdmClockPsel;
  NRF_PDM->PSEL.DIN = BoardPins::kPdmDataPsel;
  NRF_PDM->PDMCLKCTRL = PDM_PDMCLKCTRL_FREQ_Default;  // 1.032 MHz
  NRF_PDM->RATIO = PDM_RATIO_RATIO_Ratio64;           // ~16.1 kHz samples
  // SELECT strapped low on v1 -> left channel, falling edge. The wrong edge
  // samples the mic while it is tri-stated and decodes noise.
  NRF_PDM->MODE = (PDM_MODE_OPERATION_Mono << PDM_MODE_OPERATION_Pos) |
                  (PDM_MODE_EDGE_LeftFalling << PDM_MODE_EDGE_Pos);
  NRF_PDM->GAINL = 0x38;  // 0x28 = 0 dB in 0.5 dB steps -> +8 dB
  NRF_PDM->GAINR = 0x38;
  NRF_PDM->ENABLE = 1;
}

}  // namespace

bool PdmMicrophone::begin() {
  state_ = State::kUnavailable;
  timeoutAtMs_ = 0;
  discardRemaining_ = 0;
  running_ = false;

  configurePins();
  configurePeripheral();
  state_ = State::kIdle;  // a PDM mic offers no probe; validity is judged
                          // from the captured signal instead
  return true;
}

void PdmMicrophone::start(uint64_t nowMs) {
  if (state_ != State::kIdle) {
    return;
  }
  metrics_.reset();
  discardRemaining_ = kDiscardBuffers;
  fillingA = true;

  NRF_PDM->EVENTS_STARTED = 0;
  NRF_PDM->EVENTS_END = 0;
  NRF_PDM->SAMPLE.PTR = reinterpret_cast<uint32_t>(bufferA);
  NRF_PDM->SAMPLE.MAXCNT = kSamplesPerBuffer;
  NRF_PDM->TASKS_START = 1;
  running_ = true;

  // STARTED = the pointer was latched, so the second buffer can be queued.
  // From here the clock runs continuously until end().
  if (!waitEvent(NRF_PDM->EVENTS_STARTED)) {
    QUIESCO_TRACE_EVENT("mic", "start timeout", 1);
    stopPeripheral();
    state_ = State::kTimedOut;
    return;
  }
  NRF_PDM->EVENTS_STARTED = 0;
  NRF_PDM->SAMPLE.PTR = reinterpret_cast<uint32_t>(bufferB);

  timeoutAtMs_ = nowMs + kCaptureTimeoutMs;
  state_ = State::kCapturing;
}

void PdmMicrophone::poll(uint64_t nowMs) {
  if (state_ != State::kCapturing) {
    return;
  }
  if (nowMs >= timeoutAtMs_) {
    QUIESCO_TRACE_EVENT("mic", "capture timeout buffers", metrics_.buffers());
    stopPeripheral();
    state_ = State::kTimedOut;
    return;
  }
  // Both events mark a buffer boundary: END for the finished buffer, STARTED
  // for the latch of the next one. They fire microseconds apart, so requiring
  // both here only ever defers the swap to the next poll.
  if (!NRF_PDM->EVENTS_END || !NRF_PDM->EVENTS_STARTED) {
    return;
  }
  NRF_PDM->EVENTS_END = 0;
  NRF_PDM->EVENTS_STARTED = 0;

  int16_t* done = fillingA ? bufferA : bufferB;
  fillingA = !fillingA;
  // Hand the finished buffer back as the transfer-after-next target; it stays
  // untouched for the full buffer period we need to analyze it.
  NRF_PDM->SAMPLE.PTR = reinterpret_cast<uint32_t>(done);

  if (discardRemaining_ > 0) {
    discardRemaining_--;  // wake-up + filter settle, discarded while clocked
    return;
  }
  metrics_.addBuffer(done, kSamplesPerBuffer);
  if (metrics_.buffers() >= kCaptureBuffers) {
    state_ = stopPeripheral() ? State::kComplete : State::kTimedOut;
  }
}

bool PdmMicrophone::ready() const {
  return state_ == State::kUnavailable || state_ == State::kComplete ||
         state_ == State::kTimedOut;
}

void PdmMicrophone::collect(Reading& reading) {
  if (state_ != State::kComplete || !metrics_.valid()) {
    return;
  }
  reading.noiseDb = metrics_.dbA(kGainDb);
  reading.valid |= VALID_NOISE;
}

void PdmMicrophone::end() {
  if (state_ != State::kUnavailable) {
    stopPeripheral();
    NRF_PDM->ENABLE = 0;
    parkPins();
  }
  state_ = State::kUnavailable;
  timeoutAtMs_ = 0;
  discardRemaining_ = 0;
}

bool PdmMicrophone::stopPeripheral() {
  if (!running_) {
    return true;
  }
  NRF_PDM->EVENTS_STOPPED = 0;
  NRF_PDM->TASKS_STOP = 1;
  const bool stopped = waitEvent(NRF_PDM->EVENTS_STOPPED);
  running_ = false;
  return stopped;
}

bool PdmMicrophone::present() const {
  return state_ != State::kUnavailable;
}

bool PdmMicrophone::timedOut() const {
  return state_ == State::kTimedOut;
}
