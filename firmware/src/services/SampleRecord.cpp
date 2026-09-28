// SPDX-License-Identifier: GPL-3.0-only
#include "SampleRecord.h"

#include <string.h>

#include "../model/Reading.h"

uint16_t crc16Ccitt(const uint8_t* data, uint32_t length) {
  uint16_t crc = 0xFFFF;
  for (uint32_t i = 0; i < length; i++) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
  }
  return crc;
}

namespace {

uint16_t recordCrc(const SampleRecord& record) {
  SampleRecord copy;
  memcpy(&copy, &record, sizeof copy);
  copy.crc = 0;
  return crc16Ccitt(reinterpret_cast<const uint8_t*>(&copy), sizeof copy);
}

}  // namespace

void sealSampleRecord(SampleRecord& record) {
  record.crc = 0;
  record.crc = recordCrc(record);
}

bool isValidSampleRecord(const SampleRecord& record) {
  if (record.sequence == 0 ||
      record.sequence == SampleRecord::kInvalidSequenceErased) {
    return false;
  }
  return record.crc == recordCrc(record);
}

SampleRecord makeSampleRecord(const Reading& reading, uint32_t sequence,
                              uint32_t bootCount) {
  SampleRecord record;
  memset(&record, 0, sizeof record);
  record.sequence = sequence;
  record.validFlags = static_cast<uint16_t>(reading.valid);
  record.monotonicMs = reading.monotonicMs;
  record.temperatureC = reading.temperatureC;
  record.humidityPct = reading.humidityPct;
  record.pressurePa = reading.pressurePa;
  record.co2Ppm = reading.co2Ppm;
  record.lux = reading.lux;
  record.noiseDb = reading.noiseDb;
  record.batteryV = reading.batteryV;
  record.epochSeconds = static_cast<uint32_t>(reading.epochMs / 1000);
  record.bootCount = bootCount;
  if (reading.scdRhtValid) {
    const float centiC = reading.scdTemperatureC * 100.0f;
    const float centiRh = reading.scdHumidityPct * 100.0f;
    if (centiC > -32000.0f && centiC < 32000.0f && centiRh >= 0.0f &&
        centiRh < 65000.0f) {
      record.scdTemperatureCenti = static_cast<int16_t>(
          centiC + (centiC >= 0.0f ? 0.5f : -0.5f));
      record.scdHumidityCenti = static_cast<uint16_t>(centiRh + 0.5f);
      record.flags |= kSampleScdRht;
    }
  }
  sealSampleRecord(record);
  return record;
}
