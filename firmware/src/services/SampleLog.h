// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

#include "SampleRecord.h"
#include "../third_party/flashdb/flashdb.h"

class W25Q64Flash;
struct Reading;

// Read-out position for BLE record sync. The slot walk is bounded by
// remainingSlots (distance to the log head when the cursor was opened), so a
// cursor can never loop the ring.
struct SampleLogCursor {
  struct fdb_tsl_cursor physical = {};
  uint32_t generation = 0;
  uint32_t slot = 0;
  uint32_t remainingSlots = 0;
  uint32_t startSequence = 0;
  uint32_t endSequence = 0;
  bool active = false;
};

// FlashDB time-series ring of SampleRecords, using the record sequence as its
// strictly increasing database timestamp.
class SampleLog {
 public:
  // Initializes the TSDB once per boot; an erased device formats one FlashDB
  // sector and the rest stay lazy. No-op afterward.
  bool mount(W25Q64Flash& flash);

  // Fills a fixed record, assigns the next sequence, and appends atomically.
  // False on any bounded flash or database failure.
  bool append(W25Q64Flash& flash, const Reading& reading);

  uint32_t lastSequence() const { return nextSequence_ - 1; }

  // Boot counter stamped on every later record; 0 (the default) = unknown.
  void setBootCount(uint32_t bootCount) { bootCount_ = bootCount; }

  // Positions the cursor on the oldest surviving record with sequence >= the
  // request. A request older than retained history begins at the oldest.
  bool startRead(W25Q64Flash& flash, uint32_t startSequence,
                 SampleLogCursor& cursor);

  // Returns the next valid record. False means caught up or a flash failure;
  // remainingSlots == 0 distinguishes caught up from cursor.active == false.
  bool readNext(W25Q64Flash& flash, SampleLogCursor& cursor,
                SampleRecord& out);

  enum class EraseProgress : uint8_t { kRunning, kDone, kFailed };

  // Factory-reset erase of the whole partition, one sector per eraseStep()
  // so the watchdog and BLE are serviced between the ~2000 sector erases
  // (FlashDB's own clean erases everything in one ~90 s call). The database
  // is unmounted meanwhile, and sequences continue after the erase instead of
  // restarting at 1, so an app cursor never points at a reused sequence.
  // App journals the sequence floor before calling this, and resumes on boot.
  void beginErase(uint32_t nextSequence = 1);
  EraseProgress eraseStep(W25Q64Flash& flash);
  bool erasing() const { return erasing_; }

 private:
  bool initializeFlashDb(W25Q64Flash& flash);
  bool appendFlashDb(const SampleRecord& record);
  bool startReadFlashDb(uint32_t startSequence, SampleLogCursor& cursor);
  bool readNextFlashDb(SampleLogCursor& cursor, SampleRecord& out);

  struct fdb_tsdb db_ = {};
  bool mounted_ = false;
  bool erasing_ = false;
  uint32_t eraseAddress_ = 0;
  uint32_t generation_ = 0;
  uint32_t nextSequence_ = 1;
  uint32_t bootCount_ = 0;
};
