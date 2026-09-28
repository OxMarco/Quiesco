// SPDX-License-Identifier: GPL-3.0-only
#include "FlashDbPort.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "W25Q64Flash.h"
#include "../storage/FlashStorageLayout.h"
#include "../third_party/flashdb/fal_def.h"

namespace {

W25Q64Flash* selectedFlash = nullptr;
bool readFailed = false;

static_assert(W25Q64Flash::kSectorBytes == FlashStorageLayout::kSectorBytes,
              "driver and partition sector sizes differ");

int initFlash() {
  return selectedFlash == nullptr ? -1 : 0;
}

int readFlash(long offset, uint8_t* buffer, size_t size) {
  if (selectedFlash == nullptr || offset < 0 || size > UINT32_MAX) {
    return -1;
  }
  if (readFailed || !selectedFlash->read(static_cast<uint32_t>(offset), buffer,
                                        static_cast<uint32_t>(size))) {
    readFailed = true;
    memset(buffer, 0xFF, size);
    return -1;
  }
  return static_cast<int>(size);
}

int writeFlash(long offset, const uint8_t* data, size_t size) {
  if (readFailed || selectedFlash == nullptr || offset < 0 || data == nullptr ||
      size > UINT32_MAX) {
    return -1;
  }
  uint32_t address = static_cast<uint32_t>(offset);
  size_t remaining = size;
  while (remaining > 0) {
    const uint32_t pageRemaining =
        W25Q64Flash::kPageBytes - address % W25Q64Flash::kPageBytes;
    const uint32_t chunk = static_cast<uint32_t>(
        remaining < pageRemaining ? remaining : pageRemaining);
    if (!selectedFlash->program(address, data, chunk)) {
      return -1;
    }
    address += chunk;
    data += chunk;
    remaining -= chunk;
  }
  return static_cast<int>(size);
}

int eraseFlash(long offset, size_t size) {
  if (readFailed || selectedFlash == nullptr || offset < 0 || size > UINT32_MAX ||
      static_cast<uint32_t>(offset) % W25Q64Flash::kSectorBytes != 0 ||
      size % W25Q64Flash::kSectorBytes != 0) {
    return -1;
  }
  uint32_t address = static_cast<uint32_t>(offset);
  for (size_t erased = 0; erased < size;
       erased += W25Q64Flash::kSectorBytes,
              address += W25Q64Flash::kSectorBytes) {
    if (!selectedFlash->eraseSector(address)) {
      return -1;
    }
  }
  return static_cast<int>(size);
}

}  // namespace

extern "C" const struct fal_flash_dev quiesco_flash = {
    "w25q64",
    0,
    W25Q64Flash::kCapacityBytes,
    W25Q64Flash::kSectorBytes,
    {initFlash, readFlash, writeFlash, eraseFlash},
    1,
};

void selectFlashDbDevice(W25Q64Flash& flash) {
  selectedFlash = &flash;
  readFailed = false;
}

bool flashDbReadFailed() { return readFailed; }

bool initializeFlashDbKv(W25Q64Flash& flash, fdb_kvdb_t db,
                         const char* databaseName,
                         const char* partitionName) {
  selectFlashDbDevice(flash);
  memset(db, 0, sizeof *db);
  uint32_t sectorBytes = FlashStorageLayout::kSectorBytes;
  fdb_kvdb_control(db, FDB_KVDB_CTRL_SET_SEC_SIZE, &sectorBytes);
  if (fdb_kvdb_init(db, databaseName, partitionName, nullptr, nullptr) ==
      FDB_NO_ERR && !readFailed) {
    return true;
  }
  memset(db, 0, sizeof *db);
  return false;
}

bool initializeFlashDbTs(W25Q64Flash& flash, fdb_tsdb_t db,
                         const char* databaseName,
                         const char* partitionName, fdb_get_time clock,
                         size_t recordBytes) {
  selectFlashDbDevice(flash);
  memset(db, 0, sizeof *db);
  uint32_t sectorBytes = FlashStorageLayout::kSectorBytes;
  fdb_tsdb_control(db, FDB_TSDB_CTRL_SET_SEC_SIZE, &sectorBytes);
  if (fdb_tsdb_init(db, databaseName, partitionName, clock, recordBytes,
                    nullptr) == FDB_NO_ERR) {
    return true;
  }
  memset(db, 0, sizeof *db);
  return false;
}
