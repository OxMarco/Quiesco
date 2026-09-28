// SPDX-License-Identifier: GPL-3.0-only
#include "SampleLog.h"

#include <string.h>

#include "../drivers/FlashDbPort.h"

namespace {

fdb_time_t unusedClock() {
  return 0;
}

}  // namespace

bool SampleLog::initializeFlashDb(W25Q64Flash& flash) {
  if (!initializeFlashDbTs(flash, &db_, "environmental samples", "samples",
                           unusedClock, sizeof(SampleRecord))) {
    return false;
  }

  fdb_time_t last = 0;
  fdb_tsdb_control(&db_, FDB_TSDB_CTRL_GET_LAST_TIME, &last);
  if (last < 0 || last >= static_cast<fdb_time_t>(UINT32_MAX)) {
    fdb_tsdb_deinit(&db_);
    memset(&db_, 0, sizeof db_);
    return false;
  }
  // Never step backwards: after an erase the database is empty but this
  // boot's sequences carry on.
  const uint32_t recovered = static_cast<uint32_t>(last) + 1;
  if (recovered > nextSequence_) {
    nextSequence_ = recovered;
  }
  return true;
}

bool SampleLog::appendFlashDb(const SampleRecord& record) {
  ++generation_;
  struct fdb_blob blob;
  fdb_blob_make(&blob, &record, sizeof record);
  if (fdb_tsl_append_with_ts(&db_, &blob,
                             static_cast<fdb_time_t>(nextSequence_)) !=
      FDB_NO_ERR) {
    // The final commit may have reached flash even when verification failed.
    // Recover the physical tail and sequence before accepting another sample.
    fdb_tsdb_deinit(&db_);
    memset(&db_, 0, sizeof db_);
    mounted_ = false;
    return false;
  }
  nextSequence_++;
  return true;
}

bool SampleLog::startReadFlashDb(uint32_t startSequence,
                                 SampleLogCursor& cursor) {
  fdb_tsl_cursor_init(&db_, &cursor.physical, startSequence, cursor.endSequence);
  cursor.slot = startSequence;
  cursor.remainingSlots = cursor.endSequence - startSequence + 1;
  return true;
}

bool SampleLog::readNextFlashDb(SampleLogCursor& cursor, SampleRecord& out) {
  while (cursor.remainingSlots != 0) {
    struct fdb_tsl tsl;
    bool found = false;
    if (fdb_tsl_cursor_next(&db_, &cursor.physical, &tsl, &found) != FDB_NO_ERR) {
      cursor.active = false;
      return false;
    }
    if (!found) {
      cursor.remainingSlots = 0;
      return false;
    }
    struct fdb_blob blob;
    fdb_blob_make(&blob, &out, sizeof out);
    fdb_tsl_to_blob(&tsl, &blob);
    if (fdb_blob_read(reinterpret_cast<fdb_db_t>(&db_), &blob) != sizeof out) {
      cursor.active = false;
      return false;
    }
    if (!isValidSampleRecord(out) || out.sequence != tsl.time) continue;
    cursor.slot = out.sequence + 1;
    cursor.remainingSlots = cursor.endSequence - out.sequence;
    return true;
  }
  return false;
}
