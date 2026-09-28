// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

// On-flash sample record. Pure C++ (host-testable); the flash-facing side
// lives in SampleLog.
//
// The primary timestamp is the monotonic milliseconds since boot (no RTC).
// After a BLE epoch sync, records additionally carry Unix seconds in
// epochSeconds; 0 means the clock was never synced (pre-sync records, and
// every record written before this field existed, read back as unsynced).
struct SampleRecord {
  static constexpr uint32_t kInvalidSequenceErased = 0xFFFFFFFF;

  uint32_t sequence;  // 1.., strictly increasing; 0 and 0xFFFFFFFF invalid
  uint16_t crc;       // CRC-16/CCITT over the record with this field zeroed
  uint16_t validFlags;  // ReadingValid bits
  uint64_t monotonicMs;
  float temperatureC;
  float humidityPct;
  float pressurePa;
  float co2Ppm;
  float lux;
  float noiseDb;
  float batteryV;
  uint32_t epochSeconds;  // Unix seconds at capture; 0 = unsynced
  uint32_t bootCount;     // boot the record was taken in; 0 = not recorded
                          // (and every record written before this field)
  // SCD41 temperature (°C x100) and humidity (% x100) for comparison with
  // the BME280; valid only with kSampleScdRht in flags. Older records hold
  // zeros here, so they read as not recorded.
  int16_t scdTemperatureCenti;
  uint16_t scdHumidityCenti;
  uint8_t flags;
  uint8_t reserved[7];    // zero; pads the record to a power-of-two slot
};

static_assert(sizeof(SampleRecord) == 64, "Slot layout depends on 64B records");

enum SampleRecordFlags : uint8_t { kSampleScdRht = 1u << 0 };

struct Reading;

uint16_t crc16Ccitt(const uint8_t* data, uint32_t length);
void sealSampleRecord(SampleRecord& record);   // fills crc (zero the record's
                                               // reserved bytes beforehand)
bool isValidSampleRecord(const SampleRecord& record);
// Zeroed, filled and sealed from the reading; pure so the field mapping is
// host-tested apart from the flash path.
SampleRecord makeSampleRecord(const Reading& reading, uint32_t sequence,
                              uint32_t bootCount);

// Records live in the FlashDB TSDB partition (FlashStorageLayout::kSampleDb*);
// FlashDB owns slot placement, so no manual ring arithmetic lives here.
