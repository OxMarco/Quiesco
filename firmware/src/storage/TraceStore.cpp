// SPDX-License-Identifier: GPL-3.0-only
#include "TraceStore.h"

#include "../diagnostics/Trace.h"

#if QUIESCO_TRACE

#include <Arduino.h>
#include <stdio.h>

#include "../drivers/W25Q64Flash.h"
#include "FlashStorageLayout.h"

namespace TraceStore {

namespace {

constexpr uint32_t kStart = FlashStorageLayout::kTraceRingOffset;
constexpr uint32_t kEnd = FlashStorageLayout::kTraceRingEnd;
constexpr uint32_t kSector = FlashStorageLayout::kSectorBytes;
constexpr uint32_t kPage = W25Q64Flash::kPageBytes;

uint32_t head = 0;
bool headKnown = false;

bool findHead(W25Q64Flash& flash) {
  uint8_t chunk[kPage];
  for (uint32_t address = kStart; address < kEnd; address += kPage) {
    if (!flash.read(address, chunk, kPage)) {
      return false;
    }
    for (uint32_t i = 0; i < kPage; i++) {
      if (chunk[i] == 0xFF) {
        head = address + i;
        headKnown = true;
        return true;
      }
    }
  }
  // No free byte at all: not a ring this firmware wrote. Start it over.
  if (!flash.eraseSector(kStart)) {
    return false;
  }
  head = kStart;
  headKnown = true;
  return true;
}

// Keeps the invariant: the head's sector is erased from the head on.
bool advanceTo(W25Q64Flash& flash, uint32_t address) {
  if (address >= kEnd) {
    address = kStart;
  }
  head = address;
  if (head % kSector == 0) {
    return flash.eraseSector(head);
  }
  return true;
}

bool append(W25Q64Flash& flash, const uint8_t* data, uint32_t length) {
  while (length > 0) {
    const uint32_t room = kPage - head % kPage;
    const uint32_t count = length < room ? length : room;
    if (!flash.program(head, data, count)) {
      return false;
    }
    data += count;
    length -= count;
    if (!advanceTo(flash, head + count)) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool flush(W25Q64Flash& flash) {
  Trace::Buffer& buffer = Trace::buffer();
  if (buffer.used == 0 && buffer.dropped == 0) {
    return true;
  }
  if (!headKnown && !findHead(flash)) {
    return false;
  }
  if (buffer.dropped > 0) {
    char line[48];
    const int length =
        snprintf(line, sizeof line, "t=%lu trace dropped lines %lu\n",
                 static_cast<unsigned long>(millis()),
                 static_cast<unsigned long>(buffer.dropped));
    if (length > 0 && !append(flash, reinterpret_cast<const uint8_t*>(line),
                              static_cast<uint32_t>(length))) {
      headKnown = false;  // rescan next time
      return false;
    }
    buffer.dropped = 0;
  }
  const bool ok = append(flash, reinterpret_cast<const uint8_t*>(buffer.data),
                         static_cast<uint32_t>(buffer.used));
  // Kept lines are not retried: a failing flash would otherwise fill the
  // buffer and drop every new line instead.
  buffer.used = 0;
  if (!ok) {
    headKnown = false;
  }
  return ok;
}

void dump(W25Q64Flash& flash) {
  flush(flash);
  Serial.println("trace dump begin");
  uint32_t printed = 0;
  if (headKnown || findHead(flash)) {
    // Oldest first: from the sector after the head's round to the head.
    const uint32_t headSector = head - head % kSector;
    uint32_t address = headSector + kSector;
    uint8_t chunk[kPage];
    for (uint32_t done = 0; done < kEnd - kStart; done += kPage) {
      if (address >= kEnd) {
        address = kStart;
      }
      if (!flash.read(address, chunk, kPage)) {
        break;
      }
      for (uint32_t i = 0; i < kPage; i++) {
        if (chunk[i] != 0xFF) {
          Serial.write(chunk[i]);
          printed++;
        }
      }
      address += kPage;
    }
  }
  Serial.print("trace dump end bytes=");
  Serial.println(static_cast<unsigned long>(printed));
}

}  // namespace TraceStore

#else

namespace TraceStore {

bool flush(W25Q64Flash&) {
  return true;
}

void dump(W25Q64Flash&) {}

}  // namespace TraceStore

#endif
