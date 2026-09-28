// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>

#include "../third_party/flashdb/flashdb.h"

class W25Q64Flash;

// Selects the powered flash instance used by FlashDB's C callbacks. The
// application remains responsible for W25Q64Flash::begin()/end() and for the
// shared-SPI power lifecycle.
void selectFlashDbDevice(W25Q64Flash& flash);
// Latched for one operation: after a failed read, refuse every mutation.
bool flashDbReadFailed();

// Common bounded initialization shared by the storage services. Failed
// objects are reset so a caller can safely retry after the flash is powered.
bool initializeFlashDbKv(W25Q64Flash& flash, fdb_kvdb_t db,
                         const char* databaseName, const char* partitionName);
bool initializeFlashDbTs(W25Q64Flash& flash, fdb_tsdb_t db,
                         const char* databaseName, const char* partitionName,
                         fdb_get_time clock, size_t recordBytes);
