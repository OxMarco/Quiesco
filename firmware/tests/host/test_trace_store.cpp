// SPDX-License-Identifier: GPL-3.0-only
// The trace ring (storage/TraceStore), built as a trace build, over the
// in-memory W25Q64Flash sim.
#include <cstring>
#include <string>

#include "doctest.h"

#include "W25Q64FlashSim.h"
#include "diagnostics/Trace.h"
#include "drivers/W25Q64Flash.h"
#include "storage/FlashStorageLayout.h"
#include "storage/TraceStore.h"

namespace {
std::string serialOut;
}  // namespace

FakeSerial Serial;
uint32_t millis() { return 1234; }
void FakeSerial::begin(unsigned long) {}
void FakeSerial::print(const char* value) { serialOut += value; }
void FakeSerial::print(float, int) {}
void FakeSerial::println(float, int) { serialOut += '\n'; }
void FakeSerial::print(unsigned int value) { serialOut += std::to_string(value); }
void FakeSerial::print(unsigned long value) { serialOut += std::to_string(value); }
void FakeSerial::println() { serialOut += '\n'; }
void FakeSerial::println(const char* value) { serialOut += value; serialOut += '\n'; }
void FakeSerial::println(unsigned int value) { serialOut += std::to_string(value) + '\n'; }
void FakeSerial::println(unsigned long value) { serialOut += std::to_string(value) + '\n'; }
void FakeSerial::print(char value) { serialOut += value; }
void FakeSerial::print(long value) { serialOut += std::to_string(value); }
void FakeSerial::print(unsigned long long value) { serialOut += std::to_string(value); }
void FakeSerial::write(uint8_t value) { serialOut += static_cast<char>(value); }
int FakeSerial::available() { return 0; }
int FakeSerial::read() { return -1; }

TEST_CASE("trace ring recovers from an erase cut short") {
  // Power lost while the head's sector was erasing: bytes already erased
  // (0xFF) scattered among old text. The head is found at the first one and
  // the program's read-back fails on the text after it. Before the fix each
  // flush failed this way and moved on only to the next stray byte, losing
  // that cycle's lines, for as many cycles as the sector had stray bytes.
  FlashSim::reset();
  W25Q64Flash flash; flash.begin();
  uint8_t* ring = FlashSim::data() + FlashStorageLayout::kTraceRingOffset;
  std::memset(ring, 'x', FlashStorageLayout::kTraceRingEnd -
                             FlashStorageLayout::kTraceRingOffset);
  for (uint32_t i = 10; i < FlashStorageLayout::kSectorBytes; i += 2) {
    ring[i] = 0xFF;
  }

  Trace::keep("first line");
  TraceStore::flush(flash);  // may fail: it lands on the stray byte
  Trace::keep("second line");
  CHECK(TraceStore::flush(flash));
  Trace::keep("third line");
  CHECK(TraceStore::flush(flash));

  serialOut.clear();
  TraceStore::dump(flash);
  CHECK(serialOut.find("second line\nthird line\n") != std::string::npos);
  CHECK(serialOut.find("trace dump end") != std::string::npos);
}

TEST_CASE("trace ring wraps and keeps the newest lines") {
  W25Q64Flash flash; flash.begin();
  const uint32_t ringBytes =
      FlashStorageLayout::kTraceRingEnd - FlashStorageLayout::kTraceRingOffset;
  char line[64];
  uint32_t written = 0;
  for (uint32_t i = 0; written < 2 * ringBytes; ++i) {
    snprintf(line, sizeof line, "line %06u", static_cast<unsigned>(i));
    Trace::keep(line);
    written += static_cast<uint32_t>(strlen(line)) + 1;
    if (i % 100 == 99) REQUIRE(TraceStore::flush(flash));
  }
  Trace::keep("newest");
  REQUIRE(TraceStore::flush(flash));
  serialOut.clear();
  TraceStore::dump(flash);
  CHECK(serialOut.find("newest\n") != std::string::npos);
  CHECK(serialOut.find("line 000000") == std::string::npos);  // overwritten
}
