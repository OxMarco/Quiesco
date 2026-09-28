// SPDX-License-Identifier: GPL-3.0-only
#include "DeviceId.h"

#include <Arduino.h>

namespace DeviceId {

uint64_t read() {
  return (static_cast<uint64_t>(NRF_FICR->DEVICEID[1]) << 32) |
         NRF_FICR->DEVICEID[0];
}

}  // namespace DeviceId
