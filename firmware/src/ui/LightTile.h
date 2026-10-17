// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdio.h>

// The bento grid's light value, in the 76px cell at 12pt (Renderer). Kept
// apart from the renderer so the host tests can check every range.
namespace LightTile {

// Thousands are abbreviated, because a sunlit reading is six digits wide. At
// 100 klx and over even "120k lx" is 78px, past the cell, so the unit goes
// (as on the CO2 hero) rather than capping at "99k": sun on the sensor reads
// up to its rated 120 klx.
inline void format(long lux, char* out, size_t size) {
  if (lux < 1000) {
    snprintf(out, size, "%ld lx", lux < 0 ? 0 : lux);
    return;
  }
  const long k = (lux + 500) / 1000;
  snprintf(out, size, k < 100 ? "%ldk lx" : "%ldk", k);
}

}  // namespace LightTile
