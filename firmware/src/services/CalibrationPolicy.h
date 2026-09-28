// SPDX-License-Identifier: GPL-3.0-only
#pragma once

struct Config;
struct Reading;

// Applies the app-supplied calibration offsets to a collected reading, so the
// persisted, rendered, notified and synced values are all calibrated the same
// way. Only valid metrics are touched; humidity is clamped to [0, 100].
void applyCalibration(Reading& reading, const Config& config);
