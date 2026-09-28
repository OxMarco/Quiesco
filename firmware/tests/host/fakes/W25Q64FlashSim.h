// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

// Host-side control surface for the in-memory W25Q64Flash implementation in
// W25Q64FlashSim.cpp (linked in place of drivers/W25Q64Flash.cpp).
namespace FlashSim {

void failProgramAt(int call);
void failReadAt(int call);
uint32_t sampleSectorErases();
void resetReadCounts();
uint64_t readCalls();
uint64_t readBytes();
void reset();                     // erase the whole chip to 0xFF
void failNextPrograms(int count); // next N program() calls fail (slot skipped)
uint8_t* data();                  // direct backing-store access

}  // namespace FlashSim
