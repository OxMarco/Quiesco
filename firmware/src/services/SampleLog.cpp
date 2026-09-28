// SPDX-License-Identifier: GPL-3.0-only
#include "SampleLog.h"

#include <string.h>

#include "../drivers/FlashDbPort.h"
#include "../drivers/W25Q64Flash.h"
#include "../storage/FlashStorageLayout.h"

bool SampleLog::mount(W25Q64Flash& flash) {
  selectFlashDbDevice(flash);
  if (mounted_) {
    return true;
  }
  if (erasing_) {
    return false;
  }
  // A blank device formats one TSDB sector; the rest remain lazy.
  mounted_ = initializeFlashDb(flash);
  return mounted_;
}

bool SampleLog::append(W25Q64Flash& flash, const Reading& reading) {
  if (!mount(flash) || nextSequence_ == UINT32_MAX) {
    return false;
  }
  return appendFlashDb(makeSampleRecord(reading, nextSequence_, bootCount_));
}

bool SampleLog::startRead(W25Q64Flash& flash, uint32_t startSequence,
                          SampleLogCursor& cursor) {
  cursor = SampleLogCursor{};
  if (!mount(flash)) {
    return false;
  }
  cursor.active = true;
  cursor.generation = generation_;
  cursor.startSequence = startSequence;
  cursor.endSequence = lastSequence();
  if (nextSequence_ == 1 || startSequence > cursor.endSequence) {
    return true;
  }
  return startReadFlashDb(startSequence, cursor);
}

bool SampleLog::readNext(W25Q64Flash& flash, SampleLogCursor& cursor,
                         SampleRecord& out) {
  selectFlashDbDevice(flash);
  if (cursor.generation != generation_) {
    cursor.active = false;  // a write/erase invalidates this snapshot
  }
  if (!cursor.active) {
    return false;
  }
  return readNextFlashDb(cursor, out);
}

void SampleLog::beginErase(uint32_t nextSequence) {
  if (nextSequence > nextSequence_) nextSequence_ = nextSequence;
  ++generation_;
  if (mounted_) {
    fdb_tsdb_deinit(&db_);
    memset(&db_, 0, sizeof db_);
    mounted_ = false;
  }
  erasing_ = true;
  eraseAddress_ = FlashStorageLayout::kSampleDbOffset;
}

SampleLog::EraseProgress SampleLog::eraseStep(W25Q64Flash& flash) {
  if (!erasing_) {
    return EraseProgress::kFailed;
  }
  if (!flash.eraseSector(eraseAddress_)) {
    erasing_ = false;
    return EraseProgress::kFailed;
  }
  eraseAddress_ += FlashStorageLayout::kSectorBytes;
  if (eraseAddress_ < FlashStorageLayout::kSampleDbEnd) {
    return EraseProgress::kRunning;
  }
  erasing_ = false;
  return EraseProgress::kDone;
}
