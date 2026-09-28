// SPDX-License-Identifier: GPL-3.0-only
#pragma once

class EpaperDisplay;
struct UiModel;

// Draws one full frame for the model's screen. Pure presentation: no sensor,
// battery, or config access, per UI.md §6.
namespace Renderer {

bool render(EpaperDisplay& display, const UiModel& model);

}  // namespace Renderer
