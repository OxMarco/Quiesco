// SPDX-License-Identifier: GPL-3.0-only
#include "ConfigStore.h"

#include "../drivers/FlashDbPort.h"
#include "../drivers/W25Q64Flash.h"
#include "../protocol/LittleEndian.h"
#include "BondTable.h"
#include "ConfigRecord.h"

namespace {

constexpr char kConfigKey[] = "config";
constexpr char kBootCountKey[] = "boot";
constexpr char kBondsKey[] = "bonds";
constexpr char kLogEraseKey[] = "log_erase";

void advanceSequence(uint32_t& sequence) {
  sequence++;
  if (sequence == 0 || sequence == UINT32_MAX) {
    sequence = 1;
  }
}

}  // namespace

bool ConfigStore::initialize(W25Q64Flash& flash) {
  selectFlashDbDevice(flash);
  healthy_ = true;
  if (initialized_) {
    return true;
  }
  initialized_ = initializeFlashDbKv(flash, &db_, "configuration", "config");
  healthy_ = initialized_;
  return initialized_;
}

bool ConfigStore::load(W25Q64Flash& flash, Config& config) {
  if (!initialize(flash)) {
    return false;
  }

  uint8_t record[ConfigRecordLayout::kRecordBytes];
  struct fdb_blob blob;
  fdb_blob_make(&blob, record, sizeof record);
  const size_t length = fdb_kv_get_blob(&db_, kConfigKey, &blob);
  uint32_t sequence = 0;
  if (!finishOperation(true)) return false;
  if (length == sizeof record && decodeConfigRecord(record, config, sequence)) {
    nextSequence_ = sequence;
    advanceSequence(nextSequence_);
    return true;
  }
  nextSequence_ = 1;
  return false;
}

bool ConfigStore::save(W25Q64Flash& flash, const Config& config) {
  if (!isValidConfig(config) || !initialize(flash)) {
    return false;
  }
  uint8_t record[ConfigRecordLayout::kRecordBytes];
  encodeConfigRecord(config, nextSequence_, record);
  struct fdb_blob blob;
  fdb_blob_make(&blob, record, sizeof record);
  if (!finishOperation(fdb_kv_set_blob(&db_, kConfigKey, &blob) == FDB_NO_ERR)) {
    return false;
  }
  advanceSequence(nextSequence_);
  return true;
}

bool ConfigStore::advanceBootCount(W25Q64Flash& flash, uint32_t& bootCount) {
  if (!initialize(flash)) {
    return false;
  }
  uint8_t stored[4] = {};
  struct fdb_blob blob;
  fdb_blob_make(&blob, stored, sizeof stored);
  uint32_t next = 1;
  if (fdb_kv_get_blob(&db_, kBootCountKey, &blob) == sizeof stored) {
    next = LittleEndian::getU32(stored) + 1;
    if (next == 0) {
      next = 1;  // 0 means "not recorded" on the wire
    }
  }
  if (!finishOperation(true)) return false;
  LittleEndian::putU32(stored, next);
  fdb_blob_make(&blob, stored, sizeof stored);
  if (!finishOperation(fdb_kv_set_blob(&db_, kBootCountKey, &blob) == FDB_NO_ERR)) {
    return false;
  }
  bootCount = next;
  return true;
}

bool ConfigStore::loadBonds(W25Q64Flash& flash, BondTable& bonds) {
  bonds.clear();
  if (!initialize(flash)) {
    return false;
  }
  uint8_t encoded[BondTable::kEncodedBytes];
  struct fdb_blob blob;
  fdb_blob_make(&blob, encoded, sizeof encoded);
  const size_t length = fdb_kv_get_blob(&db_, kBondsKey, &blob);
  return finishOperation(true) && length == sizeof encoded && bonds.decode(encoded);
}

bool ConfigStore::saveBonds(W25Q64Flash& flash, const BondTable& bonds) {
  if (!initialize(flash)) {
    return false;
  }
  uint8_t encoded[BondTable::kEncodedBytes];
  bonds.encode(encoded);
  struct fdb_blob blob;
  fdb_blob_make(&blob, encoded, sizeof encoded);
  return finishOperation(fdb_kv_set_blob(&db_, kBondsKey, &blob) == FDB_NO_ERR);
}

bool ConfigStore::finishOperation(bool result) {
  healthy_ = !flashDbReadFailed();
  if (!healthy_ || !result) {
    // Drop caches after failed writes too: recovery must inspect physical flash.
    fdb_kvdb_deinit(&db_);
    db_ = {};
    initialized_ = false;
  }
  return healthy_ && result;
}

bool ConfigStore::loadLogErase(W25Q64Flash& flash, uint32_t& nextSequence) {
  if (!initialize(flash)) return false;
  uint8_t bytes[4] = {};
  struct fdb_blob blob;
  fdb_blob_make(&blob, bytes, sizeof bytes);
  const size_t length = fdb_kv_get_blob(&db_, kLogEraseKey, &blob);
  if (!finishOperation(true) || (length != 0 && length != sizeof bytes)) return false;
  nextSequence = length == 0 ? 0 : LittleEndian::getU32(bytes);
  return nextSequence != UINT32_MAX;
}

bool ConfigStore::saveLogErase(W25Q64Flash& flash, uint32_t nextSequence) {
  if (nextSequence == UINT32_MAX || !initialize(flash)) return false;
  uint8_t bytes[4];
  LittleEndian::putU32(bytes, nextSequence);
  struct fdb_blob blob;
  fdb_blob_make(&blob, bytes, sizeof bytes);
  return finishOperation(fdb_kv_set_blob(&db_, kLogEraseKey, &blob) == FDB_NO_ERR);
}
