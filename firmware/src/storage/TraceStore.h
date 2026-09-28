// SPDX-License-Identifier: GPL-3.0-only
#pragma once

class W25Q64Flash;

// Trace builds keep their trace lines (diagnostics/Trace.h) in a ring on the
// external flash, FlashStorageLayout::kTraceRing*, so a unit left unplugged
// from the Mac for hours can still be diagnosed: console `t` prints the ring,
// oldest line first. Other builds compile both calls to nothing.
//
// Layout: plain text lines, written in order; 0xFF marks unwritten space.
// The sector holding the write head is always erased from the head on, so
// the first 0xFF byte from the ring's start is the head, and the sector after
// it holds the oldest lines. A power cut mid-write costs at most a torn line.
namespace TraceStore {

// Appends the lines buffered since the last flush. The flash must be begun.
bool flush(W25Q64Flash& flash);

// Flushes, then prints the ring between "trace dump begin" and
// "trace dump end bytes=<n>". The flash must be begun.
void dump(W25Q64Flash& flash);

}  // namespace TraceStore
