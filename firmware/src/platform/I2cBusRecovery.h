// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Frees an I2C bus whose SDA is held low by a part left mid-byte (brownout,
// a reset during a read). Standard recovery: up to nine SCL clocks with SDA
// released, then a STOP. Bounded to well under a millisecond plus a 1 ms
// clock-stretch limit per clock. Call with the rail up and before
// Wire.begin(); the pins are handed back unconfigured for TWIM.
namespace I2cBusRecovery {

// True when SDA is high afterwards (including the usual case where it
// already was and nothing was clocked).
bool recover();

}  // namespace I2cBusRecovery
