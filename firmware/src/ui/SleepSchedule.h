// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>

#include "../model/SleepWindow.h"
#include "ComfortEvaluation.h"

// The app's judgeMode (app/src/ui/sleep.ts), ported line for line so the
// panel and the app change verdicts at the same minute. Pure C++, host-tested
// for parity against the app's own output.
namespace SleepSchedule {

// Verdicts for sleep start this long before bedtime (VERDICT_LEAD_MIN).
constexpr uint32_t kVerdictLeadMin = 60;

// Sleep from (bedtime − 60 min) until wake of the night the moment belongs
// to, day otherwise. epochMs is UTC; local time is epochMs plus the window's
// offset. The caller decides whether the window applies at all (followsApp
// and a synced clock); see panelJudgeMode.
JudgeMode judgeMode(uint64_t epochMs, const SleepWindow& window);

// What the panel uses: the app's mode when the window is followed and the
// clock is synced, otherwise kSleep at every hour (the behaviour before the
// window existed).
JudgeMode panelJudgeMode(const SleepWindow& window, bool clockSynced,
                         uint64_t epochMs);

}  // namespace SleepSchedule
