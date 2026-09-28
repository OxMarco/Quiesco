// SPDX-License-Identifier: GPL-3.0-only
#include "Renderer.h"

#include <math.h>
#include <stdio.h>

#include "../drivers/EpaperDisplay.h"
#include "UiModel.h"

// Layouts ported from the proven demo sketches (ui_face, ui_ledger, ui_bento,
// ui_battery), all in 152x152 panel pixels. Spec: UI.md §4.

namespace {

using Font = EpaperDisplay::Font;
using Ink = EpaperDisplay::Ink;

constexpr Ink kBlack = Ink::kBlack;
constexpr Ink kWhite = Ink::kWhite;

// The chassis window sits low over the panel and hides the topmost rows, so
// every layout starts below this inset. 7px is 1.27mm at the panel's 0.182
// mm/px (27.6mm active area over 152px). Nothing is drawn above it except ink
// that deliberately bleeds to the edge, such as an inverted bento hero.
constexpr int kTopInset = 7;

// ---- text helpers: '*' in a string renders as a degree ring, because the
// GFX fonts are ASCII-only ----

int measure(EpaperDisplay& d, const char* s, int degR) {
  char buf[24];
  int j = 0, extra = 0;
  for (const char* p = s; *p && j < 23; p++) {
    if (*p == '*') {
      extra += 2 * degR + 4;
    } else {
      buf[j++] = *p;
    }
  }
  buf[j] = 0;
  return d.textWidth(buf) + extra;
}

void printDeg(EpaperDisplay& d, const char* s, int degR, int degDy, Ink ink) {
  for (const char* p = s; *p; p++) {
    if (*p == '*') {
      const int x = d.cursorX(), y = d.cursorY();
      d.drawCircle(x + degR + 1, y + degDy, degR, ink);
      d.drawCircle(x + degR + 1, y + degDy, degR - 1, ink);
      d.setCursor(x + 2 * degR + 4, y);
    } else {
      d.print(*p);
    }
  }
}

void printCentered(EpaperDisplay& d, const char* s, int cx, int baseline,
                   int degR, int degDy, Ink ink) {
  d.setTextColor(ink);
  d.setCursor(cx - measure(d, s, degR) / 2, baseline);
  printDeg(d, s, degR, degDy, ink);
}

// The unit as written after a value, shared by every screen. Kept as a string
// rather than a format so the face can set the value and the unit in different
// sizes; everywhere else it is just "%ld%s".
const char* metricUnit(UiMetric metric, bool fahrenheit) {
  switch (metric) {
    case UiMetric::kCo2:
      return " ppm";
    case UiMetric::kTemperature:
      return fahrenheit ? "*F" : "*C";
    case UiMetric::kHumidity:
      return "%";
    case UiMetric::kNoise:
      return " dB";
    case UiMetric::kLight:
      return " lux";
  }
  return "";
}

// ---- screen 1: resting face ("speak only when wrong") ----

void thickCircle(EpaperDisplay& d, int cx, int cy, int r, int w) {
  d.fillCircle(cx, cy, r, kBlack);
  d.fillCircle(cx, cy, r - w, kWhite);
}

// stroke a quadratic bezier with a round brush (chunky marker look)
void brushQuad(EpaperDisplay& d, int x0, int y0, int cx, int cy, int x1,
               int y1, int r) {
  const int kSteps = 28;
  for (int i = 0; i <= kSteps; i++) {
    const float t = i / static_cast<float>(kSteps), u = 1.0f - t;
    const float x = u * u * x0 + 2 * u * t * cx + t * t * x1;
    const float y = u * u * y0 + 2 * u * t * cy + t * t * y1;
    d.fillCircle(static_cast<int>(x + 0.5f), static_cast<int>(y + 0.5f), r,
                 kBlack);
  }
}

// The reading is what you act on, so it is set 12pt while its name and unit
// stay at 9pt. A five-digit CO2 value -- only reachable with a broken sensor --
// would push the line past 152px, so the whole callout drops back to 9pt
// instead of clipping.
void printCallout(EpaperDisplay& d, const char* name, const char* value,
                  const char* unit, int cx, int base) {
  d.setFont(Font::kSemiBold9);
  const int nameW = measure(d, name, 2), unitW = measure(d, unit, 2);
  d.setFont(Font::kSemiBold12);
  int valueW = measure(d, value, 3);
  Font valueFont = Font::kSemiBold12;
  if (nameW + valueW + unitW > 148) {
    valueFont = Font::kSemiBold9;
    d.setFont(valueFont);
    valueW = measure(d, value, 2);
  }

  const int x = cx - (nameW + valueW + unitW) / 2;
  d.setTextColor(kBlack);
  d.setFont(Font::kSemiBold9);
  d.setCursor(x, base);
  d.print(name);
  d.setFont(valueFont);
  d.setCursor(x + nameW, base);
  d.print(value);  // digits and '-' only, so no degree ring to place
  d.setFont(Font::kSemiBold9);
  d.setCursor(x + nameW + valueW, base);
  printDeg(d, unit, 2, -9, kBlack);
}

const char* nudgeText(UiMetric metric, Severity severity, bool above) {
  const bool bad = severity == Severity::kBad;
  switch (metric) {
    case UiMetric::kCo2:
      return bad ? "open a window" : "getting stuffy";
    case UiMetric::kTemperature:
      if (above) return bad ? "too hot" : "a bit warm";
      return bad ? "too cold" : "a bit chilly";
    case UiMetric::kHumidity:
      if (above) return bad ? "too humid" : "a bit damp";
      return bad ? "too dry" : "a bit dry";
    case UiMetric::kNoise:
      return bad ? "too loud" : "a bit loud";
    case UiMetric::kLight:
      break;  // never drives the face
  }
  return "";
}

void drawFace(EpaperDisplay& d, const UiModel& m) {
  // No kTopInset here: the face's topmost ink is y=24, already 17px clear of
  // the chassis, so shifting it down bought nothing and crowded the nudge
  // against the bottom edge. The inset is a floor for layouts that would
  // otherwise be clipped, not a blanket translation.
  constexpr int kCy = 64;
  thickCircle(d, 76, kCy, 40, 5);
  d.fillCircle(62, kCy - 8, 4, kBlack);
  d.fillCircle(90, kCy - 8, 4, kBlack);
  if (m.faceSeverity == Severity::kOk) {
    brushQuad(d, 60, kCy + 12, 76, kCy + 29, 92, kCy + 12, 2);  // smile
  } else if (m.faceSeverity == Severity::kWarn) {
    brushQuad(d, 64, kCy + 17, 76, kCy + 17, 88, kCy + 17, 2);  // flat
  } else {
    brushQuad(d, 60, kCy + 20, 76, kCy + 6, 92, kCy + 20, 2);  // frown
  }

  if (m.faceSeverity != Severity::kOk) {
    const uint8_t worst = static_cast<uint8_t>(m.worstMetric);
    char value[12];
    snprintf(value, sizeof value, "%ld", static_cast<long>(m.values[worst]));
    // CO2 is the one reading its unit does not name, so it keeps a label.
    printCallout(d, m.worstMetric == UiMetric::kCo2 ? "CO2 " : "", value,
                 metricUnit(m.worstMetric, m.fahrenheit), 76, 126);
    d.setFont(Font::kRegular9);
    printCentered(d, nudgeText(m.worstMetric, m.faceSeverity, m.worstAbove),
                  76, 145, 2, -9, kBlack);
  }
}

// ---- screen 2: ledger with comfort bands ----

constexpr int kGaugeX0 = 10, kGaugeX1 = 142;

struct LedgerRow {
  UiMetric metric;
  const char* label;
  float lo, hi;   // full gauge scale
  bool logScale;  // lux spans decades
};

// Judged metrics first, CO2 at the top because it drives most of the advice;
// light last because it is the one row that carries no verdict.
constexpr LedgerRow kLedgerRows[kUiMetricCount] = {
    {UiMetric::kCo2, "CO2", 400, 2000, false},
    {UiMetric::kTemperature, "TEMP", 10, 35, false},
    {UiMetric::kHumidity, "HUMIDITY", 0, 100, false},
    {UiMetric::kNoise, "NOISE", 30, 100, false},
    {UiMetric::kLight, "LIGHT", 1, 10000, true},
};

// A 1-bit panel has no lighter ink, so the full-scale track is dotted to make
// it recede behind the solid comfort band drawn over it.
void dottedHLine(EpaperDisplay& d, int x0, int x1, int y) {
  for (int x = x0; x < x1; x += 2) {
    d.fillRect(x, y, 1, 1, kBlack);
  }
}

float gaugeFrac(const LedgerRow& r, float v) {
  const float f = r.logScale
                      ? (log10f(v) - log10f(r.lo)) / (log10f(r.hi) - log10f(r.lo))
                      : (v - r.lo) / (r.hi - r.lo);
  return f < 0 ? 0 : (f > 1 ? 1 : f);
}

int gaugeX(const LedgerRow& r, float v) {
  return kGaugeX0 + static_cast<int>((kGaugeX1 - kGaugeX0) * gaugeFrac(r, v) +
                                     0.5f);
}

// The ledger runs 7pt where the other screens run 9pt: it is the only layout
// putting a label and a value on one 132px line, and at 9pt a five-digit lux or
// CO2 reading collided with its label. 7pt leaves 27px clear at the worst case.
void drawLedgerRow(EpaperDisplay& d, const UiModel& m, const LedgerRow& r,
                   int base) {
  d.setFont(Font::kRegular7);
  d.setTextColor(kBlack);
  d.setCursor(kGaugeX0, base);
  d.print(r.label);

  const uint8_t i = static_cast<uint8_t>(r.metric);
  const bool valid = m.validMask & (1u << i);
  char val[20];
  if (valid) {
    snprintf(val, sizeof val, "%ld%s", static_cast<long>(m.values[i]),
             metricUnit(r.metric, m.fahrenheit));
  } else {
    snprintf(val, sizeof val, "n/a");
  }
  d.setFont(Font::kSemiBold7);
  d.setCursor(kGaugeX1 - measure(d, val, 2), base);
  printDeg(d, val, 2, -7, kBlack);  // ring top-aligned with the 10px digits

  const int y = base + 9;
  dottedHLine(d, kGaugeX0, kGaugeX1, y);  // full scale

  // Light is the exception: ComfortEvaluation never judges it, so drawing it a
  // band would put the marker outside one in a bright room and read as a fault
  // that no other screen agrees with.
  if (r.metric != UiMetric::kLight) {
    const MetricBand& band = metricBand(r.metric);
    const float bandLo = band.okLo < r.lo ? r.lo : band.okLo;
    const float bandHi = band.okHi > r.hi ? r.hi : band.okHi;
    const int bx0 = gaugeX(r, bandLo), bx1 = gaugeX(r, bandHi);
    d.fillRoundRect(bx0, y - 1, bx1 - bx0, 3, 1, kBlack);  // comfort band
  }

  if (valid) {
    // The gauge and its band are drawn in °C.
    float v = static_cast<float>(m.values[i]);
    if (r.metric == UiMetric::kTemperature && m.fahrenheit) {
      v = (v - 32.0f) * 5.0f / 9.0f;
    }
    const int mx = gaugeX(r, v);
    d.fillCircle(mx, y, 4, kBlack);  // "you are here"
    d.fillCircle(mx, y, 2, kWhite);
  }
}

void drawLedger(EpaperDisplay& d, const UiModel& m) {
  // 5 rows at 28px pitch below the inset; the last marker bottoms out at y=148.
  for (uint8_t i = 0; i < kUiMetricCount; i++) {
    drawLedgerRow(d, m, kLedgerRows[i], kTopInset + 16 + 28 * i);
  }
}

// ---- screen 3: bento grid (readings only, no face) ----

// Tile values carry their own unit, which keeps every label a bare metric name
// that fits the 76px cell -- "NOISE dB" as a label does not. Two exceptions:
// the CO2 hero prints digits only (the 24pt cut carries no letters) and light
// abbreviates thousands, because a sunlit reading is six digits wide.
void bentoValue(UiMetric metric, long value, bool fahrenheit, char* out,
                size_t size) {
  if (metric == UiMetric::kCo2) {
    snprintf(out, size, "%ld", value);
  } else if (metric == UiMetric::kLight) {
    if (value < 1000) {
      snprintf(out, size, "%ld lx", value);
    } else {
      const long k = (value + 500) / 1000;
      snprintf(out, size, "%ldk lx", k > 99 ? 99 : k);
    }
  } else {
    snprintf(out, size, "%ld%s", value, metricUnit(metric, fahrenheit));
  }
}

void drawBentoTile(EpaperDisplay& d, const UiModel& m, UiMetric metric,
                   const char* label, int x, int y, int w, int h,
                   Font valueFont, int valueBase, int labelBase, int degR,
                   int degDy) {
  const uint8_t i = static_cast<uint8_t>(metric);
  const bool valid = m.validMask & (1u << i);
  char val[20];
  if (valid) {
    bentoValue(metric, static_cast<long>(m.values[i]), m.fahrenheit, val,
               sizeof val);
  } else {
    snprintf(val, sizeof val, "--");
  }

  Ink fg = kBlack;
  if (valid && m.severities[i] != Severity::kOk) {  // out of band -> invert
    d.fillRect(x, y, w, h, kBlack);
    fg = kWhite;
  }
  const int cx = x + w / 2;
  d.setFont(valueFont);
  printCentered(d, val, cx, y + valueBase, degR, degDy, fg);
  d.setFont(Font::kRegular9);
  d.setTextColor(fg);
  d.setCursor(cx - d.textWidth(label) / 2, y + labelBase);
  d.print(label);
}

void drawBento(EpaperDisplay& d, const UiModel& m) {
  // Hero band over a 2x2 grid, dividers at 60 and 106 rather than 56 and 104:
  // the hero needs room for a 33px numeral that now starts below the inset, and
  // the bottom row still has to end on the panel. Tile rects stay edge to edge
  // so an inverted hero bleeds into the hidden rows instead of leaving a white
  // sliver there; only the baselines respect the inset. Hero digits occupy
  // 9-41 and its label 45-57; each grid tile leaves 5px above and below its
  // value+label pair in the 45 rows under its divider row.
  d.drawFastHLine(0, 60, 152, kBlack);
  d.drawFastHLine(0, 106, 152, kBlack);
  d.drawFastVLine(76, 60, 92, kBlack);

  drawBentoTile(d, m, UiMetric::kCo2, "CO2 PPM", 0, 0, 152, 60,
                Font::kSemiBold24, 41, 57, 0, 0);
  drawBentoTile(d, m, UiMetric::kTemperature, "TEMP", 0, 60, 76, 46,
                Font::kSemiBold12, 22, 40, 3, -13);
  drawBentoTile(d, m, UiMetric::kHumidity, "HUMID", 76, 60, 76, 46,
                Font::kSemiBold12, 22, 40, 3, -13);
  drawBentoTile(d, m, UiMetric::kLight, "LIGHT", 0, 106, 76, 46,
                Font::kSemiBold12, 22, 40, 3, -13);
  drawBentoTile(d, m, UiMetric::kNoise, "NOISE", 76, 106, 76, 46,
                Font::kSemiBold12, 22, 40, 3, -13);
}

// ---- screen 4: battery (charging or nearly empty) ----

void drawBolt(EpaperDisplay& d, int cx, int cy, Ink ink) {
  d.fillTriangle(cx + 10, cy - 22, cx - 14, cy + 4, cx + 2, cy + 4, ink);
  d.fillTriangle(cx - 10, cy + 22, cx + 14, cy - 4, cx - 2, cy - 4, ink);
}

void drawBattery(EpaperDisplay& d, const UiModel& m) {
  const int x = 14, y = 47 + kTopInset, w = 116, h = 58;
  const int stroke = 5, inset = stroke + 3;
  const uint8_t percent = m.batteryPercent > 100 ? 100 : m.batteryPercent;

  // Positive terminal first so its left edge disappears behind the body.
  d.fillRoundRect(x + w - 1, y + 18, 9, 22, 3, kBlack);
  d.fillRoundRect(x, y, w, h, 8, kBlack);
  d.fillRoundRect(x + stroke, y + stroke, w - 2 * stroke, h - 2 * stroke, 4,
                  kWhite);

  const int cavityW = w - 2 * inset;
  const int fillW = (cavityW * percent + 50) / 100;
  if (fillW > 0) {
    d.fillRect(x + inset, y + inset, fillW, h - 2 * inset, kBlack);
  }

  if (m.charging) {
    // White bolt with a black outline stays visible across the fill edge.
    const int cx = x + w / 2, cy = y + h / 2;
    drawBolt(d, cx - 2, cy, kBlack);
    drawBolt(d, cx + 2, cy, kBlack);
    drawBolt(d, cx, cy - 2, kBlack);
    drawBolt(d, cx, cy + 2, kBlack);
    drawBolt(d, cx, cy, kWhite);
  }
}

void drawUnavailable(EpaperDisplay& d) {
  d.setFont(Font::kSemiBold12);
  printCentered(d, "readings", 76, 72 + kTopInset, 2, -13, kBlack);
  printCentered(d, "unavailable", 76, 98 + kTopInset, 2, -13, kBlack);
  d.setFont(Font::kRegular9);
  printCentered(d, "check sensors", 76, 120 + kTopInset, 1, -5, kBlack);
}

// ---- setup: the six-digit code stays on the physical display ----
// Two rows of three: six 24pt digits are ~150px, too wide for one line.
void drawPairing(EpaperDisplay& d, const UiModel& m) {
  char line[4];
  d.setFont(Font::kSemiBold12);
  printCentered(d, "phone setup", 76, 18 + kTopInset, 0, 0, kBlack);
  d.setFont(Font::kSemiBold24);
  snprintf(line, sizeof line, "%03lu",
           static_cast<unsigned long>(m.pairingCode / 1000 % 1000));
  printCentered(d, line, 76, 66 + kTopInset, 0, 0, kBlack);
  snprintf(line, sizeof line, "%03lu",
           static_cast<unsigned long>(m.pairingCode % 1000));
  printCentered(d, line, 76, 106 + kTopInset, 0, 0, kBlack);
  d.setFont(Font::kRegular7);
  printCentered(d, "enter code on phone", 76, 132 + kTopInset, 0, 0, kBlack);
}

}  // namespace

namespace Renderer {

bool render(EpaperDisplay& display, const UiModel& model) {
  display.beginFrame();
  switch (model.screen) {
    case ScreenId::kFace:
      drawFace(display, model);
      break;
    case ScreenId::kLedger:
      drawLedger(display, model);
      break;
    case ScreenId::kBento:
      drawBento(display, model);
      break;
    case ScreenId::kBattery:
      drawBattery(display, model);
      break;
    case ScreenId::kUnavailable:
      drawUnavailable(display);
      break;
    case ScreenId::kPairing:
      drawPairing(display, model);
      break;
  }
  return display.endFrame();
}

}  // namespace Renderer
