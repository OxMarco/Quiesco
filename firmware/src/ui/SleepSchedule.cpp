// SPDX-License-Identifier: GPL-3.0-only
#include "SleepSchedule.h"

namespace SleepSchedule {

namespace {

constexpr int64_t kMinuteMs = 60000;
constexpr int64_t kDayMs = 24 * 60 * kMinuteMs;
constexpr int64_t kNoonMin = 12 * 60;

// Floor division: a night or weekday of a negative local time stays correct.
int64_t floorDiv(int64_t a, int64_t b) {
  const int64_t q = a / b;
  return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

int64_t wrapMinutes(int64_t minutes) {
  const int64_t day = SleepWindow::kMinutesPerDay;
  return ((minutes % day) + day) % day;
}

// Nights are named by the local day number of the evening that starts them.
// 1970-01-01 was a Thursday (getDay() 4); Friday and Saturday nights are the
// weekend (isWeekendNight).
bool isWeekendNight(int64_t night) {
  const int64_t weekday = ((night + 4) % 7 + 7) % 7;
  return weekday == 5 || weekday == 6;
}

struct Window {
  int64_t fromMs;  // local
  int64_t toMs;
};

// windowForNight: a bedtime before noon falls in the small hours after the
// evening; the window lasts the wrapped wake − bed.
Window windowForNight(int64_t night, const SleepWindow& window) {
  const bool weekend = window.hasWeekend() && isWeekendNight(night);
  const int64_t bed = weekend ? window.weekendBedMin : window.weekdayBedMin;
  const int64_t wake = weekend ? window.weekendWakeMin : window.weekdayWakeMin;
  const int64_t day = bed < kNoonMin ? night + 1 : night;
  Window out;
  out.fromMs = day * kDayMs + bed * kMinuteMs;
  out.toMs = out.fromMs + wrapMinutes(wake - bed) * kMinuteMs;
  return out;
}

}  // namespace

JudgeMode judgeMode(uint64_t epochMs, const SleepWindow& window) {
  const int64_t localMs = static_cast<int64_t>(epochMs) +
                          int64_t{window.utcOffsetMinutes} * kMinuteMs;
  // nightOf: the moment's local date 12 h earlier.
  const int64_t night = floorDiv(localMs - kDayMs / 2, kDayMs);
  Window current = windowForNight(night, window);
  if (localMs >= current.toMs) {
    current = windowForNight(night + 1, window);
  }
  if (localMs >= current.fromMs) {
    return JudgeMode::kSleep;
  }
  // The app rounds the minutes to bedtime (Math.round) before comparing them
  // with the lead: round(x) <= 60 holds exactly while x < 60.5 minutes.
  const int64_t leadMs =
      static_cast<int64_t>(kVerdictLeadMin) * kMinuteMs + kMinuteMs / 2;
  return current.fromMs - localMs < leadMs ? JudgeMode::kSleep
                                           : JudgeMode::kDay;
}

JudgeMode panelJudgeMode(const SleepWindow& window, bool clockSynced,
                         uint64_t epochMs) {
  if (!window.followsApp() || !clockSynced || epochMs == 0) {
    return JudgeMode::kSleep;
  }
  return judgeMode(epochMs, window);
}

}  // namespace SleepSchedule
