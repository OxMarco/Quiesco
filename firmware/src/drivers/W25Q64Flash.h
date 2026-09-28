// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

// W25Q64JV on the shared SPI bus, using only the bounded command set proven
// by 02_test_flash (no filesystem, per SOFTWARE.md §2). The e-ink CS is
// parked HIGH before any transaction; every wait-for-idle has a deadline.
//
// Lifecycle: begin() only while the peripheral rail is ready (it releases the
// chip from deep power-down and verifies the JEDEC ID), end() puts it back to
// deep power-down before the rail drops.
class W25Q64Flash {
 public:
  static constexpr uint32_t kCapacityBytes = 8UL * 1024 * 1024;
  static constexpr uint32_t kSectorBytes = 4096;
  static constexpr uint32_t kPageBytes = 256;
  // W25Q64JV datasheet maxima, rounded up: page program 3 ms, sector erase
  // 400 ms.
  static constexpr uint32_t kProgramTimeoutMs = 10;
  static constexpr uint32_t kEraseTimeoutMs = 500;

  bool begin();
  void end();
  bool present() const { return present_; }
  bool timedOut() const { return timedOut_; }

  // len is bounded by the caller; read may cross pages freely. program must
  // stay inside one 256-byte page (the chip wraps otherwise). Both return
  // false on a missing chip or an expired deadline; program verifies by
  // reading back.
  bool read(uint32_t address, uint8_t* buffer, uint32_t length);
  bool program(uint32_t address, const uint8_t* data, uint32_t length);
  bool eraseSector(uint32_t address);

 private:
  bool waitIdle(uint32_t timeoutMs);

  bool present_ = false;
  bool timedOut_ = false;
};
