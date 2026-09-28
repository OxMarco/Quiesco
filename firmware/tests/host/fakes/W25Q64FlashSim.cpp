// SPDX-License-Identifier: GPL-3.0-only
// In-memory implementation of the W25Q64Flash class for host tests. Mirrors
// the physical constraints that matter to callers: programming can only clear
// bits (program-without-erase fails the read-back verify), program must stay
// inside one 256-byte page, and erase works on whole 4 KB sectors.
#include "W25Q64FlashSim.h"

#include <string.h>

#include <vector>

#include "drivers/W25Q64Flash.h"
#include "storage/FlashStorageLayout.h"

namespace {

std::vector<uint8_t>& storage() {
  static std::vector<uint8_t> bytes(W25Q64Flash::kCapacityBytes, 0xFF);
  return bytes;
}

int failPrograms = 0;
int programCountdown = 0;
int readCountdown = 0;
uint64_t reads = 0, bytesRead = 0;
uint32_t sampleErases = 0;

}  // namespace

namespace FlashSim {

void reset() {
  memset(storage().data(), 0xFF, storage().size());
  failPrograms = 0;
  programCountdown = readCountdown = 0;
  reads = bytesRead = 0;
  sampleErases = 0;
}

uint32_t sampleSectorErases() { return sampleErases; }

void failProgramAt(int call) { programCountdown = call; }
void failReadAt(int call) { readCountdown = call; }
void resetReadCounts() { reads = bytesRead = 0; }
uint64_t readCalls() { return reads; }
uint64_t readBytes() { return bytesRead; }

void failNextPrograms(int count) { failPrograms = count; }

uint8_t* data() { return storage().data(); }

}  // namespace FlashSim

bool W25Q64Flash::begin() {
  present_ = true;
  timedOut_ = false;
  return true;
}

void W25Q64Flash::end() {}

bool W25Q64Flash::read(uint32_t address, uint8_t* buffer, uint32_t length) {
  if (!present_ || address + length > kCapacityBytes) {
    return false;
  }
  ++reads; bytesRead += length;
  if (readCountdown > 0 && --readCountdown == 0) return false;
  memcpy(buffer, storage().data() + address, length);
  return true;
}

bool W25Q64Flash::program(uint32_t address, const uint8_t* data,
                          uint32_t length) {
  if (!present_ || address + length > kCapacityBytes ||
      address / kPageBytes != (address + length - 1) / kPageBytes) {
    return false;
  }
  if (programCountdown > 0 && --programCountdown == 0) failPrograms = 1;
  if (failPrograms > 0) {
    failPrograms--;
    // A torn program may clear only bits requested by the command; it cannot
    // invent arbitrary cleared bits that were 1 in the transmitted byte.
    storage()[address] &= data[0];
    return false;
  }
  bool verified = true;
  for (uint32_t i = 0; i < length; i++) {
    storage()[address + i] &= data[i];  // programming only clears bits
    verified = verified && storage()[address + i] == data[i];
  }
  return verified;
}

bool W25Q64Flash::eraseSector(uint32_t address) {
  if (!present_ || address % kSectorBytes != 0 || address >= kCapacityBytes) {
    return false;
  }
  if (address >= FlashStorageLayout::kSampleDbOffset &&
      address < FlashStorageLayout::kSampleDbEnd) ++sampleErases;
  memset(storage().data() + address, 0xFF, kSectorBytes);
  return true;
}

bool W25Q64Flash::waitIdle(uint32_t) { return true; }
