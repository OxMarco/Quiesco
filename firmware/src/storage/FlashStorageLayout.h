// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// C-compatible constants are consumed by FlashDB's FAL table. Keep every
// physical partition boundary here so the C and C++ storage layers cannot
// silently drift apart.
#define QUIESCO_FLASH_SECTOR_BYTES 0x001000UL
// 0x000000-0x002000 is unpartitioned. The offsets below are left where they
// are rather than compacted: nothing needs the two sectors, and moving a
// partition would orphan the config on any device already flashed on the bench.
#define QUIESCO_CONFIG_DB_OFFSET 0x002000UL
#define QUIESCO_CONFIG_DB_BYTES 0x002000UL
// Trace builds only (diagnostics/Trace.h): a ring of text lines in the gap
// between the two partitions, which neither FlashDB database touches.
#define QUIESCO_TRACE_RING_OFFSET 0x004000UL
#define QUIESCO_TRACE_RING_END 0x010000UL
#define QUIESCO_SAMPLE_DB_OFFSET 0x010000UL
#define QUIESCO_SAMPLE_DB_END 0x7FF000UL
#define QUIESCO_SAMPLE_DB_BYTES                                           \
  (QUIESCO_SAMPLE_DB_END - QUIESCO_SAMPLE_DB_OFFSET)

#ifdef __cplusplus
#include <stdint.h>

namespace FlashStorageLayout {

constexpr uint32_t kSectorBytes = QUIESCO_FLASH_SECTOR_BYTES;
constexpr uint32_t kConfigDbOffset = QUIESCO_CONFIG_DB_OFFSET;
constexpr uint32_t kConfigDbBytes = QUIESCO_CONFIG_DB_BYTES;
constexpr uint32_t kTraceRingOffset = QUIESCO_TRACE_RING_OFFSET;
constexpr uint32_t kTraceRingEnd = QUIESCO_TRACE_RING_END;
constexpr uint32_t kSampleDbOffset = QUIESCO_SAMPLE_DB_OFFSET;
constexpr uint32_t kSampleDbEnd = QUIESCO_SAMPLE_DB_END;
constexpr uint32_t kSampleDbBytes = QUIESCO_SAMPLE_DB_BYTES;
// The last sector belongs to no partition: the factory test (smoke/sensors)
// erases and programs it, so it can exercise the chip on a unit that already
// holds a config and a log without touching either. smoke/sensors asserts
// that it is the chip's last sector.
constexpr uint32_t kTestSectorOffset = kSampleDbEnd;

static_assert(kConfigDbOffset + kConfigDbBytes <= kSampleDbOffset,
              "configuration and sample partitions overlap");
static_assert(kConfigDbOffset + kConfigDbBytes <= kTraceRingOffset &&
                  kTraceRingEnd <= kSampleDbOffset,
              "trace ring overlaps a partition");
static_assert(kTraceRingOffset % kSectorBytes == 0 &&
                  kTraceRingEnd % kSectorBytes == 0 &&
                  kTraceRingEnd - kTraceRingOffset >= 2 * kSectorBytes,
              "trace ring must be at least two whole sectors");
static_assert(kSampleDbBytes % kSectorBytes == 0,
              "sample partition must contain whole sectors");

}  // namespace FlashStorageLayout
#endif
