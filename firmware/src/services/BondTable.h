// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

// Four enrolled phones. The v2 table stores a key id in the address bytes and
// a random app-authentication key in LTK. IRK fields remain for layout stability.
// Enrollment evicts the oldest entry only after proof of the on-screen key.
// Legacy v1 tables are rejected: their keys were transmitted in cleartext.
struct Bond {
  uint8_t address[6];  // identity address, byte order as ArduinoBLE gives it
  uint8_t irk[16];     // peer identity resolving key
  uint8_t ltk[16];     // long-term key
  uint32_t age;        // larger = more recent; 0 = empty slot
  bool hasIrk;
  bool hasLtk;
};

class BondTable {
 public:
  static constexpr uint8_t kMaxBonds = 4;
  // Version byte, count, then per bond: address, IRK, LTK, flags; CRC-16.
  static constexpr uint32_t kEncodedBytes = 2 + kMaxBonds * (6 + 16 + 16 + 1) + 2;

  void clear();
  void storeIrk(const uint8_t address[6], const uint8_t irk[16]);
  void storeLtk(const uint8_t address[6], const uint8_t ltk[16]);
  bool findLtk(const uint8_t address[6], uint8_t ltk[16]) const;

  uint8_t count() const;  // occupied slots
  // Occupied slots with an IRK, in slot order; index < irkCount().
  uint8_t irkCount() const;
  const Bond* irkBond(uint8_t index) const;

  void encode(uint8_t out[kEncodedBytes]) const;
  // False (table untouched) on a bad version or CRC.
  bool decode(const uint8_t in[kEncodedBytes]);

 private:
  Bond* slotFor(const uint8_t address[6]);

  Bond bonds_[kMaxBonds] = {};
  uint32_t nextAge_ = 1;
};
