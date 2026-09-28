// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

#include "../model/Config.h"
#include "../third_party/flashdb/flashdb.h"

class BondTable;
class W25Q64Flash;

// Power-loss-safe FlashDB KV store. After a false load(), check healthy():
// a storage failure must be retried, never replaced with default settings.
class ConfigStore {
 public:
  bool load(W25Q64Flash& flash, Config& config);
  bool save(W25Q64Flash& flash, const Config& config);
  // Increments the persisted boot counter (1 on a blank store) and returns
  // the new value; call once per boot. False leaves bootCount untouched.
  bool advanceBootCount(W25Q64Flash& flash, uint32_t& bootCount);
  // Bonded phones, in the same power-loss-safe store. loadBonds() false
  // means none stored or unreadable; the table is left empty.
  bool loadBonds(W25Q64Flash& flash, BondTable& bonds);
  bool saveBonds(W25Q64Flash& flash, const BondTable& bonds);

  bool healthy() const { return healthy_; }
  // 0 means no pending erase; otherwise the first sequence after deletion.
  bool loadLogErase(W25Q64Flash& flash, uint32_t& nextSequence);
  bool saveLogErase(W25Q64Flash& flash, uint32_t nextSequence);

 private:
  bool finishOperation(bool result);
  bool initialize(W25Q64Flash& flash);

  struct fdb_kvdb db_ = {};
  bool initialized_ = false;
  bool healthy_ = false;
  uint32_t nextSequence_ = 1;
};
