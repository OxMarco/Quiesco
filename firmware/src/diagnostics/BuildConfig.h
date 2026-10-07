// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#ifndef QUIESCO_DEBUG
#define QUIESCO_DEBUG 0
#endif

#if QUIESCO_DEBUG
constexpr bool kDebugEnabled = true;
#else
constexpr bool kDebugEnabled = false;
#endif

// Raw ArduinoBLE HCI trace on USB serial (build.sh --debug --ble-trace). Very
// verbose; only for diagnosing pairing and link problems.
#ifndef QUIESCO_BLE_TRACE
#define QUIESCO_BLE_TRACE 0
#endif

// Bench units without a cell (build.sh --no-battery). Production units always
// have one. With no cell the charger's output swings between about 3.7 and
// 4.2 V and ~CHG means nothing, and the unit cannot tell that from a real
// battery, so this build reports no battery and never shows the battery screen.
#ifndef QUIESCO_NO_BATTERY
#define QUIESCO_NO_BATTERY 0
#endif

constexpr bool kBatteryFitted = !QUIESCO_NO_BATTERY;

// BLE OTA update (device control opcode 3, capability bit 15). Off: the XIAO
// bootloader accepts the update but never finishes erasing the old image
// (PROTOCOL.md §9.3), which leaves a unit that needs USB. Updates go
// through the XIAO-BOOT drive instead (AGENT.md).
#ifndef QUIESCO_BLE_OTA
#define QUIESCO_BLE_OTA 0
#endif

constexpr bool kBleOtaEnabled = QUIESCO_BLE_OTA;
