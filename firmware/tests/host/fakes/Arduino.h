// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

#define D0 0
#define D1 1
#define D11 30
#define D15 34
#define PIN_VBAT_ENABLE 29

constexpr uint8_t LOW = 0;
constexpr uint8_t HIGH = 1;
constexpr uint8_t INPUT = 0;
constexpr uint8_t OUTPUT = 1;

void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t value);
void delay(unsigned long milliseconds);
uint32_t millis();

class FakeSerial {
 public:
  void begin(unsigned long baud);
  void print(const char* value);
  void print(float value, int decimals);
  void print(unsigned int value);
  void print(unsigned long value);
  void println();
  void println(const char* value);
  void println(unsigned int value);
  void println(unsigned long value);
  void print(char value);
  void print(long value);
  void print(unsigned long long value);
  int available();
  int read();
};

extern FakeSerial Serial;
