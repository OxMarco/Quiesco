// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

// Adapter over the e-ink backend (GxEPD2 for development, per the license
// gate in SOFTWARE.md §8). No backend type crosses this header, so the
// renderer and application stay backend-independent and host-checkable.
//
// Lifecycle: begin() only while the peripheral rail is ready, end() before
// the rail goes down. One frame = beginFrame() + primitives + endFrame().
class EpaperDisplay {
 public:
  static constexpr int16_t kWidth = 152;
  static constexpr int16_t kHeight = 152;

  enum class Font : uint8_t {
    kRegular7,
    kSemiBold7,
    kRegular9,
    kSemiBold9,
    kSemiBold12,
    kSemiBold24,
  };
  enum class Ink : uint8_t { kWhite, kBlack };

  bool begin(bool fullRefresh);
  bool end();  // hibernate the panel and report a BUSY timeout

  void beginFrame();                 // full window, cleared to white
  bool endFrame();                   // bounded full or differential refresh
  bool timedOut() const;
  bool present() const;

  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, Ink ink);
  void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
                     Ink ink);
  void fillCircle(int16_t x, int16_t y, int16_t r, Ink ink);
  void drawCircle(int16_t x, int16_t y, int16_t r, Ink ink);
  void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2,
                    int16_t y2, Ink ink);
  void drawFastHLine(int16_t x, int16_t y, int16_t w, Ink ink);
  void drawFastVLine(int16_t x, int16_t y, int16_t h, Ink ink);

  void setFont(Font font);
  void setTextColor(Ink ink);
  void setCursor(int16_t x, int16_t y);
  int16_t cursorX() const;
  int16_t cursorY() const;
  void print(const char* text);
  void print(char character);
  int16_t textWidth(const char* text);
};
