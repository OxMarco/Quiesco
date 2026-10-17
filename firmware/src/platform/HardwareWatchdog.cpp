// SPDX-License-Identifier: GPL-3.0-only
#include "HardwareWatchdog.h"

#include <Arduino.h>

void HardwareWatchdog::begin() {
  if (NRF_WDT->RUNSTATUS == WDT_RUNSTATUS_RUNSTATUS_NotRunning) {
    NRF_WDT->CONFIG =
        (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos) |
        (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos);
    NRF_WDT->CRV = (kTimeoutMs * 32768UL) / 1000UL;
    NRF_WDT->RREN = WDT_RREN_RR0_Enabled << WDT_RREN_RR0_Pos;
    NRF_WDT->TASKS_START = 1;
  }
  active_ = (NRF_WDT->RREN & WDT_RREN_RR0_Msk) != 0;
}

void HardwareWatchdog::kick() {
  if (active_) {
    NRF_WDT->RR[0] = WDT_RR_RR_Reload;
  }
}

// FlashDB's whole-log format (fdb_tsdb.c), a finite run of sector erases
// that takes minutes: reload between erases so it can finish. A reload
// write is ignored while the watchdog is not running.
extern "C" void fdb_quiesco_long_operation_step(void) {
  NRF_WDT->RR[0] = WDT_RR_RR_Reload;
}
