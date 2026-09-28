// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Driver-level trace for diagnosing a faulty unit: raw sensor values, I2C and
// Sensirion error codes, retries, panel BUSY timings. Build with
// `make build-trace` (build.sh --debug --trace) and watch it with
// `scripts/quiesco-console.py monitor`. One line per event:
//
//   t=<ms since boot> trace <source> <what> <value>
//
// QUIESCO_TRACE_EVENT lines are also kept in a flash ring (storage/
// TraceStore) that survives unplugging; console `t` prints it. Together with
// the cycle and event lines DebugLog adds, it covers a unit left alone for
// hours. QUIESCO_TRACE_FLOAT is serial only.
//
// Off by default; the macros then compile to nothing, so a release or plain
// debug image carries no trace code. Header-only, so the drivers can use it
// in the smoke sketches too, which do not link DebugLog.

#ifndef QUIESCO_TRACE
#define QUIESCO_TRACE 0
#endif

#if QUIESCO_TRACE

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

namespace Trace {

// Lines wait here until the next measurement cycle's flush writes them to the
// trace ring on the external flash (storage/TraceStore), so a unit away from
// the Mac keeps its history. Full buffer: new lines are counted, not kept.
constexpr size_t kBufferBytes = 4096;

struct Buffer {
  char data[kBufferBytes];
  size_t used = 0;
  uint32_t dropped = 0;
};

inline Buffer& buffer() {
  static Buffer instance;
  return instance;
}

// One line, no newline, kept for the flash ring only.
inline void keep(const char* line) {
  Buffer& b = buffer();
  const size_t length = strlen(line);
  if (b.used + length + 1 > kBufferBytes) {
    b.dropped++;
    return;
  }
  memcpy(b.data + b.used, line, length);
  b.used += length;
  b.data[b.used++] = '\n';
}

// The same, also printed on USB serial.
inline void record(const char* line) {
  Serial.println(line);
  keep(line);
}

inline void event(const char* source, const char* what, long value) {
  char line[112];
  snprintf(line, sizeof line, "t=%lu trace %s %s %ld",
           static_cast<unsigned long>(millis()), source, what, value);
  record(line);
}

// Serial only: for values the sample log already keeps (BME280, CO2, lux).
inline void eventFloat(const char* source, const char* what, float value) {
  Serial.print("t=");
  Serial.print(millis());
  Serial.print(" trace ");
  Serial.print(source);
  Serial.print(' ');
  Serial.print(what);
  Serial.print(' ');
  Serial.println(value, 2);
}

}  // namespace Trace

#define QUIESCO_TRACE_EVENT(source, what, value) \
  ::Trace::event((source), (what), static_cast<long>(value))
#define QUIESCO_TRACE_FLOAT(source, what, value) \
  ::Trace::eventFloat((source), (what), static_cast<float>(value))

#else

#define QUIESCO_TRACE_EVENT(source, what, value) \
  do {                                           \
  } while (0)
#define QUIESCO_TRACE_FLOAT(source, what, value) \
  do {                                           \
  } while (0)

#endif
