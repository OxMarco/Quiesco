// SPDX-License-Identifier: GPL-3.0-only
#include "EpaperDisplay.h"

#include <Adafruit_GFX.h>
#include <GxEPD2_BW.h>

#include "../ui/fonts/FredokaRegular7pt.h"
#include "../ui/fonts/FredokaRegular9pt.h"
#include "../ui/fonts/FredokaSemiBold7pt.h"
#include "../ui/fonts/FredokaSemiBold9pt.h"
#include "../ui/fonts/FredokaSemiBold12pt.h"
#include "../ui/fonts/FredokaSemiBold24pt.h"

#include "../board/BoardPins.h"
#include "../diagnostics/Trace.h"

namespace {

constexpr uint32_t kFrameBytes =
    EpaperDisplay::kWidth * EpaperDisplay::kHeight / 8;

// The panel loses both differential-update planes when its shared rail drops.
// Keep the previous plane in MCU RAM and restore it before each partial update.
// This preserves rail gating without forcing a full refresh for every change.
class FrameBuffer : public Adafruit_GFX {
 public:
  FrameBuffer() : Adafruit_GFX(EpaperDisplay::kWidth, EpaperDisplay::kHeight) {
    clear();
  }

  void clear() { memset(data_, 0xFF, sizeof data_); }
  const uint8_t* data() const { return data_; }

  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (x < 0 || x >= width() || y < 0 || y >= height()) {
      return;
    }
    switch (getRotation()) {
      case 1:
        swap(x, y);
        x = WIDTH - x - 1;
        break;
      case 2:
        x = WIDTH - x - 1;
        y = HEIGHT - y - 1;
        break;
      case 3:
        swap(x, y);
        y = HEIGHT - y - 1;
        break;
    }
    uint8_t& byte = data_[x / 8 + y * (WIDTH / 8)];
    const uint8_t mask = 1u << (7 - (x & 7));
    byte = color == 0 ? byte & ~mask : byte | mask;
  }

 private:
  template <typename T>
  static void swap(T& a, T& b) {
    const T value = a;
    a = b;
    b = value;
  }

  static constexpr int16_t WIDTH = EpaperDisplay::kWidth;
  static constexpr int16_t HEIGHT = EpaperDisplay::kHeight;
  uint8_t data_[kFrameBytes];
};

GxEPD2_154_T8 panel(BoardPins::kEpaperCs, BoardPins::kEpaperDc,
                     BoardPins::kEpaperReset, BoardPins::kEpaperBusy);
FrameBuffer frame;
uint8_t previousFrame[kFrameBytes];
bool previousFrameValid = false;
bool fullRefresh = true;
bool busyTimedOut = false;
bool sawBusy = false;
bool responsive = false;

void busyObserved(const void*) {
  sawBusy = true;
  delay(1);
}

// GxEPD2's init() pulses RST, waits 10 ms and returns without looking at
// BUSY, but the controller holds BUSY low while it comes out of reset (longer
// when it was hibernating). Sampling BUSY at once read that as a timeout and
// skipped the frame: the likely cause of WORKPLAN M1's display=timeout on the
// first cycle after a reflash. A generous bound still catches a stuck line.
constexpr uint32_t kResetBusyTimeoutMs = 200;

void waitForResetBusy() {
  const uint32_t startMs = millis();
  while (digitalRead(BoardPins::kEpaperBusy) == LOW &&
         millis() - startMs < kResetBusyTimeoutMs) {
    delay(1);
  }
  const uint32_t waitedMs = millis() - startMs;
  if (waitedMs > 0) {
    QUIESCO_TRACE_EVENT("display", "reset busy ms", waitedMs);
  }
}

bool busyReleased() {
  // GDEW0154T8 BUSY is active LOW. GxEPD2 bounds the wait; checking the pin
  // when it returns distinguishes a completed operation from that timeout.
  const bool released = digitalRead(BoardPins::kEpaperBusy) != LOW;
  if (!released) {
    QUIESCO_TRACE_EVENT("display", "busy still low", 1);
  }
  busyTimedOut = busyTimedOut || !released;
  return released;
}

uint16_t toColor(EpaperDisplay::Ink ink) {
  return ink == EpaperDisplay::Ink::kBlack ? GxEPD_BLACK : GxEPD_WHITE;
}

}  // namespace

static_assert(EpaperDisplay::kWidth == GxEPD2_154_T8::WIDTH &&
                  EpaperDisplay::kHeight == GxEPD2_154_T8::HEIGHT,
              "Adapter geometry out of sync with the panel driver");

bool EpaperDisplay::begin(bool useFullRefresh) {
  // The flash shares the SPI bus; its CS must stay parked while we drive it.
  digitalWrite(BoardPins::kFlashCs, HIGH);
  fullRefresh = useFullRefresh || !previousFrameValid;
  busyTimedOut = false;
  sawBusy = false;
  responsive = false;
  panel.init(0, fullRefresh);
  waitForResetBusy();
  panel.setBusyCallback(busyObserved, nullptr);
  frame.setRotation(2);
  return busyReleased();
}

bool EpaperDisplay::end() {
  // Check BUSY before deep sleep, not after: in deep sleep the UC8151 holds
  // BUSY low until the next reset, so sampling it after hibernate() reported
  // a timeout on every draw (WORKPLAN M1). That failed the frame, so the next
  // cycle forced another full refresh (~2.2 s) and the same false timeout.
  // powerOff() waits for BUSY; hibernate()'s own repeat of it is harmless.
  panel.powerOff();
  const bool released = busyReleased();
  panel.hibernate();
  // Never call SPI.end() on this core: MbedSPI::end() deletes the bus object
  // without nulling it, so the next begin() skips re-creation and every later
  // transfer runs on freed memory (HARDWARE.md §6.2). The object persists across
  // cycles instead; rail-off pin states are PowerDomain's concern.
  return released;
}

void EpaperDisplay::beginFrame() {
  frame.clear();
}

bool EpaperDisplay::endFrame() {
  const uint32_t startMs = millis();
  if (fullRefresh) {
    panel.writeImageForFullRefresh(frame.data(), 0, 0, kWidth, kHeight);
    panel.refresh(false);
    panel.writeImageAgain(frame.data(), 0, 0, kWidth, kHeight);
    panel.powerOff();
  } else {
    panel.writeImageToPrevious(previousFrame, 0, 0, kWidth, kHeight);
    panel.writeImage(frame.data(), 0, 0, kWidth, kHeight);
    panel.refresh(true);
    panel.writeImageAgain(frame.data(), 0, 0, kWidth, kHeight);
  }
  QUIESCO_TRACE_EVENT("display",
                      fullRefresh ? "full refresh ms" : "partial refresh ms",
                      millis() - startMs);
  if (!sawBusy) {
    QUIESCO_TRACE_EVENT("display", "never saw busy", 1);
  }
  (void)startMs;  // trace-only
  responsive = sawBusy;
  if (!busyReleased() || !responsive) {
    return false;
  }
  memcpy(previousFrame, frame.data(), sizeof previousFrame);
  previousFrameValid = true;
  return true;
}

void EpaperDisplay::fillRect(int16_t x, int16_t y, int16_t w, int16_t h,
                             Ink ink) {
  frame.fillRect(x, y, w, h, toColor(ink));
}

void EpaperDisplay::fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h,
                                  int16_t r, Ink ink) {
  frame.fillRoundRect(x, y, w, h, r, toColor(ink));
}

void EpaperDisplay::fillCircle(int16_t x, int16_t y, int16_t r, Ink ink) {
  frame.fillCircle(x, y, r, toColor(ink));
}

void EpaperDisplay::drawCircle(int16_t x, int16_t y, int16_t r, Ink ink) {
  frame.drawCircle(x, y, r, toColor(ink));
}

void EpaperDisplay::fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                 int16_t x2, int16_t y2, Ink ink) {
  frame.fillTriangle(x0, y0, x1, y1, x2, y2, toColor(ink));
}

void EpaperDisplay::drawFastHLine(int16_t x, int16_t y, int16_t w, Ink ink) {
  frame.drawFastHLine(x, y, w, toColor(ink));
}

void EpaperDisplay::drawFastVLine(int16_t x, int16_t y, int16_t h, Ink ink) {
  frame.drawFastVLine(x, y, h, toColor(ink));
}

void EpaperDisplay::setFont(Font font) {
  switch (font) {
    case Font::kRegular7:
      frame.setFont(&Fredoka_Regular7pt7b);
      frame.setTextSize(1);
      break;
    case Font::kSemiBold7:
      frame.setFont(&Fredoka_SemiBold7pt7b);
      frame.setTextSize(1);
      break;
    case Font::kRegular9:
      frame.setFont(&Fredoka_Regular9pt8b);
      frame.setTextSize(1);
      break;
    case Font::kSemiBold9:
      frame.setFont(&Fredoka_SemiBold9pt8b);
      frame.setTextSize(1);
      break;
    case Font::kSemiBold12:
      frame.setFont(&Fredoka_SemiBold12pt8b);
      frame.setTextSize(1);
      break;
    case Font::kSemiBold24:
      // A real cut, not the 12pt bitmap at setTextSize(2): pixel doubling also
      // doubles the stroke weight and quantises the curves. Digits only, so it
      // costs ~1.2KB instead of ~16KB; nothing but the bento hero uses it.
      frame.setFont(&Fredoka_SemiBold24pt7b);
      frame.setTextSize(1);
      break;
  }
}

void EpaperDisplay::setTextColor(Ink ink) {
  frame.setTextColor(toColor(ink));
}

void EpaperDisplay::setCursor(int16_t x, int16_t y) {
  frame.setCursor(x, y);
}

int16_t EpaperDisplay::cursorX() const {
  return frame.getCursorX();
}

int16_t EpaperDisplay::cursorY() const {
  return frame.getCursorY();
}

void EpaperDisplay::print(const char* text) {
  frame.print(text);
}

void EpaperDisplay::print(char character) {
  frame.print(character);
}

int16_t EpaperDisplay::textWidth(const char* text) {
  int16_t x = 0, y = 0;
  uint16_t w = 0, h = 0;
  frame.getTextBounds(text, 0, 0, &x, &y, &w, &h);
  return static_cast<int16_t>(w);
}

bool EpaperDisplay::timedOut() const {
  return busyTimedOut;
}

bool EpaperDisplay::present() const {
  return responsive;
}
