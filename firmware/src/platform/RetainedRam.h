// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

#include "../services/ResetHistory.h"

// A few bytes in the linker's .noinit section, which startup code never
// clears, so they outlive every reset but a power loss. Each item carries a
// magic number and a checksum: after a power-on, or if the bootloader reused
// the RAM, it reads as absent rather than as garbage.
namespace RetainedRam {

// Reads what the previous boot left, then clears it so a later boot never
// reads it again. hasBreadcrumb and hasFault say which items were valid;
// the others are left untouched.
void take(ResetHistory::Breadcrumb& breadcrumb, bool& hasBreadcrumb,
          ResetHistory::FaultCapture& fault, bool& hasFault);

// Called at every watchdog feed. A reset mid-write leaves a bad checksum,
// which the next boot reads as no breadcrumb.
void store(const ResetHistory::Breadcrumb& breadcrumb);

// Heap in use (newlib mallinfo), KiB, rounded up.
uint16_t heapKiB();

}  // namespace RetainedRam
