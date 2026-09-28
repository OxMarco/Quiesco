#pragma once

// Quiesco FlashDB 2.2.0 configuration.
#define FDB_USING_FAL_MODE
#define FDB_USING_KVDB
#define FDB_USING_TSDB
#define FDB_USING_TIMESTAMP_64BIT
#define FDB_TSDB_FIXED_BLOB_SIZE 64
#define FDB_WRITE_GRAN 1

// Bound the RAM cost of the configuration database.
#define FDB_KV_CACHE_TABLE_SIZE 4
#define FDB_SECTOR_CACHE_TABLE_SIZE 2

// Storage failures flow through the application's fault model.
#define FDB_PRINT(...) ((void)0)
