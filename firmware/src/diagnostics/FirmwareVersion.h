// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "BuildConfig.h"

// Firmware version, the single source for DIS, the BLE device-info payload and
// the debug boot line. scripts/build.sh sets the three components from the
// repository's VERSION file; any other build (host tests, the Arduino IDE)
// reports 0.0.0 so an unversioned image can never pass for a release.
#ifndef QUIESCO_VERSION_MAJOR
#define QUIESCO_VERSION_MAJOR 0
#endif
#ifndef QUIESCO_VERSION_MINOR
#define QUIESCO_VERSION_MINOR 0
#endif
#ifndef QUIESCO_VERSION_PATCH
#define QUIESCO_VERSION_PATCH 0
#endif

#define QUIESCO_VERSION_STR_(x) #x
#define QUIESCO_VERSION_STR(x) QUIESCO_VERSION_STR_(x)

namespace FirmwareVersion {

constexpr unsigned kMajor = QUIESCO_VERSION_MAJOR;
constexpr unsigned kMinor = QUIESCO_VERSION_MINOR;
constexpr unsigned kPatch = QUIESCO_VERSION_PATCH;

static_assert(kMajor <= 255 && kMinor <= 255 && kPatch <= 255,
              "each version component travels as one byte over BLE");

// "1.2.3", or "1.2.3-debug" for a QUIESCO_DEBUG image.
constexpr char kString[] =
    QUIESCO_VERSION_STR(QUIESCO_VERSION_MAJOR) "." QUIESCO_VERSION_STR(
        QUIESCO_VERSION_MINOR) "." QUIESCO_VERSION_STR(QUIESCO_VERSION_PATCH)
#if QUIESCO_DEBUG
        "-debug"
#endif
    ;

}  // namespace FirmwareVersion
