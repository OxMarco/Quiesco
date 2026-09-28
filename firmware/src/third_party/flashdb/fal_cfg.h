#pragma once

#include "../../storage/FlashStorageLayout.h"

#ifdef __cplusplus
extern "C" {
#endif

struct fal_flash_dev;
extern const struct fal_flash_dev quiesco_flash;

#ifdef __cplusplus
}
#endif

#define FAL_PART_HAS_TABLE_CFG
#define FAL_FLASH_DEV_TABLE {&quiesco_flash}

#define FAL_PART_TABLE                                                        \
  {                                                                           \
    {FAL_PART_MAGIC_WORD, "config", "w25q64", QUIESCO_CONFIG_DB_OFFSET,      \
     QUIESCO_CONFIG_DB_BYTES, 0},                                             \
        {FAL_PART_MAGIC_WORD, "samples", "w25q64",                          \
         QUIESCO_SAMPLE_DB_OFFSET, QUIESCO_SAMPLE_DB_BYTES, 0},               \
  }

#define FAL_PRINTF(...) ((void)0)
#define FAL_DEBUG 0
