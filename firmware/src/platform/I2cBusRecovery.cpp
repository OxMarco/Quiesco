// SPDX-License-Identifier: GPL-3.0-only
#include "I2cBusRecovery.h"

#include <Arduino.h>

#include "../board/BoardPins.h"
#include "../diagnostics/Trace.h"

namespace I2cBusRecovery {

namespace {

constexpr uint32_t kHalfPeriodUs = 5;        // 100 kHz, the bus's own rate
constexpr uint32_t kStretchLimitUs = 1000;
constexpr uint8_t kMaxClocks = 9;

constexpr uint32_t kSda = BoardPins::kI2cSdaPortPin;
constexpr uint32_t kScl = BoardPins::kI2cSclPortPin;

// Open-drain (S0D1): OUTSET releases a line to the pull-ups, OUTCLR pulls it
// low, the same contract as I2C itself.
void configureOpenDrain(uint32_t pin) {
  NRF_P0->OUTSET = 1u << pin;
  NRF_P0->PIN_CNF[pin] =
      (GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos) |
      (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos) |
      (GPIO_PIN_CNF_PULL_Disabled << GPIO_PIN_CNF_PULL_Pos) |
      (GPIO_PIN_CNF_DRIVE_S0D1 << GPIO_PIN_CNF_DRIVE_Pos) |
      (GPIO_PIN_CNF_SENSE_Disabled << GPIO_PIN_CNF_SENSE_Pos);
}

void release(uint32_t pin) {
  NRF_P0->PIN_CNF[pin] =
      (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos) |
      (GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos);
}

void set(uint32_t pin, bool high) {
  if (high) {
    NRF_P0->OUTSET = 1u << pin;
  } else {
    NRF_P0->OUTCLR = 1u << pin;
  }
}

bool read(uint32_t pin) {
  return ((NRF_P0->IN >> pin) & 1u) != 0;
}

// Releases SCL and waits, bounded, for a stretching part to let go.
bool sclHigh() {
  set(kScl, true);
  const uint32_t startUs = micros();
  while (!read(kScl)) {
    if (micros() - startUs > kStretchLimitUs) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool recover() {
  configureOpenDrain(kSda);
  configureOpenDrain(kScl);
  delayMicroseconds(kHalfPeriodUs);
  bool freed = read(kSda);
  if (!freed) {
    QUIESCO_TRACE_EVENT("i2c", "sda held low", 1);
  }
  for (uint8_t clock = 0; clock < kMaxClocks && !freed; clock++) {
    set(kScl, false);
    delayMicroseconds(kHalfPeriodUs);
    if (!sclHigh()) {
      break;  // SCL itself is held: nothing clocking can fix
    }
    delayMicroseconds(kHalfPeriodUs);
    freed = read(kSda);
  }
  if (freed) {
    // STOP: SDA rises while SCL is high, ending any half-done transfer.
    set(kScl, false);
    set(kSda, false);
    delayMicroseconds(kHalfPeriodUs);
    sclHigh();
    delayMicroseconds(kHalfPeriodUs);
    set(kSda, true);
    delayMicroseconds(kHalfPeriodUs);
    freed = read(kSda);
  }
  release(kSda);
  release(kScl);
  if (!freed) {
    QUIESCO_TRACE_EVENT("i2c", "recovery failed", 1);
  }
  return freed;
}

}  // namespace I2cBusRecovery
