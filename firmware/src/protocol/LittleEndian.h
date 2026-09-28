// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>
#include <string.h>

// Explicit little-endian packing for everything that leaves the MCU (BLE
// payloads, on-flash config records), so the wire layout is defined by these
// functions rather than by struct padding or host endianness.
namespace LittleEndian {

inline void putU16(uint8_t* out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
}

inline void putU32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
  out[2] = static_cast<uint8_t>(value >> 16);
  out[3] = static_cast<uint8_t>(value >> 24);
}

inline void putU64(uint8_t* out, uint64_t value) {
  putU32(out, static_cast<uint32_t>(value));
  putU32(out + 4, static_cast<uint32_t>(value >> 32));
}

inline void putI16(uint8_t* out, int16_t value) {
  putU16(out, static_cast<uint16_t>(value));
}

inline void putF32(uint8_t* out, float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof bits);
  putU32(out, bits);
}

inline uint16_t getU16(const uint8_t* in) {
  return static_cast<uint16_t>(in[0]) | static_cast<uint16_t>(in[1]) << 8;
}

inline uint32_t getU32(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) | static_cast<uint32_t>(in[1]) << 8 |
         static_cast<uint32_t>(in[2]) << 16 |
         static_cast<uint32_t>(in[3]) << 24;
}

inline uint64_t getU64(const uint8_t* in) {
  return static_cast<uint64_t>(getU32(in)) |
         static_cast<uint64_t>(getU32(in + 4)) << 32;
}

inline int16_t getI16(const uint8_t* in) {
  return static_cast<int16_t>(getU16(in));
}

inline float getF32(const uint8_t* in) {
  const uint32_t bits = getU32(in);
  float value;
  memcpy(&value, &bits, sizeof value);
  return value;
}

}  // namespace LittleEndian
