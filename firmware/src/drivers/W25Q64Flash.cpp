// SPDX-License-Identifier: GPL-3.0-only
#include "W25Q64Flash.h"

#include <Arduino.h>
#include <SPI.h>

#include "../board/BoardPins.h"
#include "../diagnostics/Trace.h"

namespace {

const SPISettings kSpiSettings(8000000, MSBFIRST, SPI_MODE0);

void csLow() {
  digitalWrite(BoardPins::kFlashCs, LOW);
}
void csHigh() {
  digitalWrite(BoardPins::kFlashCs, HIGH);
}

void sendAddress(uint32_t address) {
  SPI.transfer((address >> 16) & 0xFF);
  SPI.transfer((address >> 8) & 0xFF);
  SPI.transfer(address & 0xFF);
}

uint8_t readStatus1() {
  SPI.beginTransaction(kSpiSettings);
  csLow();
  SPI.transfer(0x05);
  const uint8_t status = SPI.transfer(0x00);
  csHigh();
  SPI.endTransaction();
  return status;
}

// False when the chip did not latch WEL (status bit 1): a lost command or a
// write-protected chip, which would ignore the program or erase that follows
// and leave the old contents.
bool writeEnable() {
  SPI.beginTransaction(kSpiSettings);
  csLow();
  SPI.transfer(0x06);
  csHigh();
  SPI.endTransaction();
  return (readStatus1() & 0x02) != 0;
}

}  // namespace

bool W25Q64Flash::begin() {
  present_ = false;
  timedOut_ = false;

  // The e-ink shares the bus; its CS must stay parked while we drive it.
  digitalWrite(BoardPins::kEpaperCs, HIGH);
  SPI.begin();  // idempotent; SPI.end() is forbidden (HARDWARE.md §6.2)

  // Release from deep power-down in case a previous cycle left it there.
  SPI.beginTransaction(kSpiSettings);
  csLow();
  SPI.transfer(0xAB);
  csHigh();
  SPI.endTransaction();
  delayMicroseconds(50);  // tRES1

  SPI.beginTransaction(kSpiSettings);
  csLow();
  SPI.transfer(0x9F);
  const uint8_t manufacturer = SPI.transfer(0);
  const uint8_t type = SPI.transfer(0);
  const uint8_t capacity = SPI.transfer(0);
  csHigh();
  SPI.endTransaction();

  // 00 00 00 = no power (the rail), FF FF FF = not selected/clocked (the bus).
  present_ = manufacturer == 0xEF && type == 0x40 && capacity == 0x17;
  if (!present_) {
    QUIESCO_TRACE_EVENT("flash", "jedec id",
                        static_cast<uint32_t>(manufacturer) << 16 |
                            static_cast<uint32_t>(type) << 8 | capacity);
  }
  return present_;
}

void W25Q64Flash::end() {
  if (present_) {
    SPI.beginTransaction(kSpiSettings);
    csLow();
    SPI.transfer(0xB9);  // deep power-down until the next release
    csHigh();
    SPI.endTransaction();
    // App ends the flash last before the rail drops. MOSI can hold the last
    // bit sent, and 0xB9 ends in a 1, which would feed the unpowered flash
    // and panel through their input protection diodes until the next cycle.
    // A zero byte with nothing selected leaves it low.
    SPI.beginTransaction(kSpiSettings);
    SPI.transfer(0x00);
    SPI.endTransaction();
  }
  present_ = false;
}

bool W25Q64Flash::waitIdle(uint32_t timeoutMs) {
  const uint32_t startMs = millis();
  while (readStatus1() & 0x01) {  // WIP
    if (millis() - startMs > timeoutMs) {
      timedOut_ = true;
      return false;
    }
    delayMicroseconds(100);
  }
  return true;
}

bool W25Q64Flash::read(uint32_t address, uint8_t* buffer, uint32_t length) {
  if (!present_ || buffer == nullptr || address >= kCapacityBytes ||
      length > kCapacityBytes - address) {
    return false;
  }
  SPI.beginTransaction(kSpiSettings);
  csLow();
  SPI.transfer(0x03);
  sendAddress(address);
  for (uint32_t i = 0; i < length; i++) {
    buffer[i] = SPI.transfer(0x00);
  }
  csHigh();
  SPI.endTransaction();
  return true;
}

bool W25Q64Flash::program(uint32_t address, const uint8_t* data,
                          uint32_t length) {
  if (!present_ || data == nullptr || length == 0 ||
      address >= kCapacityBytes || length > kCapacityBytes - address ||
      length > kPageBytes ||
      (address % kPageBytes) + length > kPageBytes) {
    return false;
  }
  if (!writeEnable()) {
    return false;
  }
  SPI.beginTransaction(kSpiSettings);
  csLow();
  SPI.transfer(0x02);
  sendAddress(address);
  for (uint32_t i = 0; i < length; i++) {
    SPI.transfer(data[i]);
  }
  csHigh();
  SPI.endTransaction();
  if (!waitIdle(kProgramTimeoutMs)) {
    return false;
  }

  uint8_t verify[kPageBytes];
  if (!read(address, verify, length)) {
    return false;
  }
  for (uint32_t i = 0; i < length; i++) {
    if (verify[i] != data[i]) {
      return false;
    }
  }
  return true;
}

bool W25Q64Flash::eraseSector(uint32_t address) {
  if (!present_ || address >= kCapacityBytes ||
      address % kSectorBytes != 0) {
    return false;
  }
  if (!writeEnable()) {
    QUIESCO_TRACE_EVENT("flash", "erase not enabled", address);
    return false;
  }
  SPI.beginTransaction(kSpiSettings);
  csLow();
  SPI.transfer(0x20);
  sendAddress(address);
  csHigh();
  SPI.endTransaction();
  if (!waitIdle(kEraseTimeoutMs)) {
    return false;
  }
  // Program has its read-back verify; erase gets a blank check of its first
  // page, where FlashDB keeps the sector header. A sector that reported
  // success but kept its data would otherwise pass as erased: a log wipe
  // that left records, or a header written over old bytes.
  uint8_t head[kPageBytes];
  if (!read(address, head, sizeof head)) {
    return false;
  }
  for (uint8_t byte : head) {
    if (byte != 0xFF) {
      QUIESCO_TRACE_EVENT("flash", "erase not blank", address);
      return false;
    }
  }
  return true;
}
