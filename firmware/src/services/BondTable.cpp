// SPDX-License-Identifier: GPL-3.0-only
#include "BondTable.h"

#include <string.h>

#include "../protocol/LittleEndian.h"
#include "SampleRecord.h"  // crc16Ccitt

namespace {

// v1 keys crossed the radio in cleartext. Require physical re-enrollment.
constexpr uint8_t kEncodingVersion = 2;
constexpr uint8_t kFlagIrk = 1u << 0;
constexpr uint8_t kFlagLtk = 1u << 1;
constexpr uint32_t kEntryBytes = 6 + 16 + 16 + 1;

}  // namespace

void BondTable::clear() {
  memset(bonds_, 0, sizeof bonds_);
  nextAge_ = 1;
}

Bond* BondTable::slotFor(const uint8_t address[6]) {
  Bond* oldest = &bonds_[0];
  for (Bond& bond : bonds_) {
    if (bond.age != 0 && memcmp(bond.address, address, 6) == 0) {
      return &bond;
    }
  }
  for (Bond& bond : bonds_) {
    if (bond.age == 0) {
      oldest = &bond;
      break;
    }
    if (bond.age < oldest->age) {
      oldest = &bond;
    }
  }
  memset(oldest, 0, sizeof *oldest);  // new or evicted
  memcpy(oldest->address, address, 6);
  return oldest;
}

void BondTable::storeIrk(const uint8_t address[6], const uint8_t irk[16]) {
  Bond* bond = slotFor(address);
  memcpy(bond->irk, irk, 16);
  bond->hasIrk = true;
  bond->age = nextAge_++;
}

void BondTable::storeLtk(const uint8_t address[6], const uint8_t ltk[16]) {
  Bond* bond = slotFor(address);
  memcpy(bond->ltk, ltk, 16);
  bond->hasLtk = true;
  bond->age = nextAge_++;
}

bool BondTable::findLtk(const uint8_t address[6], uint8_t ltk[16]) const {
  for (const Bond& bond : bonds_) {
    if (bond.age != 0 && bond.hasLtk && memcmp(bond.address, address, 6) == 0) {
      memcpy(ltk, bond.ltk, 16);
      return true;
    }
  }
  return false;
}

uint8_t BondTable::count() const {
  uint8_t n = 0;
  for (const Bond& bond : bonds_) {
    n += bond.age != 0 ? 1 : 0;
  }
  return n;
}

uint8_t BondTable::irkCount() const {
  uint8_t n = 0;
  for (const Bond& bond : bonds_) {
    n += (bond.age != 0 && bond.hasIrk) ? 1 : 0;
  }
  return n;
}

const Bond* BondTable::irkBond(uint8_t index) const {
  for (const Bond& bond : bonds_) {
    if (bond.age != 0 && bond.hasIrk && index-- == 0) {
      return &bond;
    }
  }
  return nullptr;
}

// Slots are written oldest first, so decode() rebuilds the same eviction
// order without storing the ages themselves.
void BondTable::encode(uint8_t out[kEncodedBytes]) const {
  memset(out, 0, kEncodedBytes);
  out[0] = kEncodingVersion;
  uint32_t offset = 2;
  uint32_t lastAge = 0;
  uint8_t written = 0;
  for (uint8_t n = 0; n < kMaxBonds; n++) {
    const Bond* next = nullptr;
    for (const Bond& bond : bonds_) {
      if (bond.age > lastAge && (next == nullptr || bond.age < next->age)) {
        next = &bond;
      }
    }
    if (next == nullptr) {
      break;
    }
    lastAge = next->age;
    memcpy(out + offset, next->address, 6);
    memcpy(out + offset + 6, next->irk, 16);
    memcpy(out + offset + 22, next->ltk, 16);
    out[offset + 38] = static_cast<uint8_t>((next->hasIrk ? kFlagIrk : 0) |
                                            (next->hasLtk ? kFlagLtk : 0));
    offset += kEntryBytes;
    written++;
  }
  out[1] = written;
  LittleEndian::putU16(out + kEncodedBytes - 2,
                       crc16Ccitt(out, kEncodedBytes - 2));
}

bool BondTable::decode(const uint8_t in[kEncodedBytes]) {
  if (in[0] != kEncodingVersion || in[1] > kMaxBonds ||
      LittleEndian::getU16(in + kEncodedBytes - 2) !=
          crc16Ccitt(in, kEncodedBytes - 2)) {
    return false;
  }
  clear();
  uint32_t offset = 2;
  for (uint8_t i = 0; i < in[1]; i++) {
    Bond& bond = bonds_[i];
    memcpy(bond.address, in + offset, 6);
    memcpy(bond.irk, in + offset + 6, 16);
    memcpy(bond.ltk, in + offset + 22, 16);
    bond.hasIrk = (in[offset + 38] & kFlagIrk) != 0;
    bond.hasLtk = (in[offset + 38] & kFlagLtk) != 0;
    bond.age = nextAge_++;
    offset += kEntryBytes;
  }
  return true;
}
