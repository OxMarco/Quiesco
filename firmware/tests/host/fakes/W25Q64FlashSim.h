// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

// Host-side control surface for the in-memory W25Q64Flash implementation in
// W25Q64FlashSim.cpp (linked in place of drivers/W25Q64Flash.cpp).
namespace FlashSim {

void failProgramAt(int call);
void failReadAt(int call);
// The Nth sample-log sector erase from now fails and leaves the sector as it
// was: power lost before the erase began.
void failSampleEraseAt(int call);
uint32_t sampleSectorErases();
void resetReadCounts();
uint64_t readCalls();
uint64_t readBytes();
void reset();                     // erase the whole chip to 0xFF
void failNextPrograms(int count); // next N program() calls fail (slot skipped)
uint8_t* data();                  // direct backing-store access
// false: begin() reads no JEDEC ID, as with a dead chip or open bus.
void setChipAnswers(bool answers);
uint32_t begins();                // W25Q64Flash::begin() calls since reset()

}  // namespace FlashSim
