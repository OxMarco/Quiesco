// SPDX-License-Identifier: GPL-3.0-only
#include "RetainedRam.h"

#include <Arduino.h>
#include <malloc.h>
#include <string.h>

#include "platform/internal/mbed_fault_handler.h"
#include "platform/mbed_error.h"

namespace {

constexpr uint32_t kBreadcrumbMagic = 0x51424331;  // "QBC1"
constexpr uint32_t kFaultMagic = 0x51464331;       // "QFC1"

// FNV-1a over the payload, seeded with the magic.
uint32_t checksum(uint32_t magic, const void* data, size_t length) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  uint32_t hash = 2166136261u ^ magic;
  for (size_t i = 0; i < length; ++i) {
    hash = (hash ^ bytes[i]) * 16777619u;
  }
  return hash;
}

// Raw bytes, not a T: T's member initializers would give the slot a
// constructor, and static initialization would wipe it before take().
template <typename T>
struct Slot {
  uint32_t magic;
  alignas(8) uint8_t data[sizeof(T)];
  uint32_t check;
};

__attribute__((section(".noinit"))) Slot<ResetHistory::Breadcrumb> breadcrumbSlot;
__attribute__((section(".noinit"))) Slot<ResetHistory::FaultCapture> faultSlot;

template <typename T>
bool takeSlot(Slot<T>& slot, uint32_t magic, T& out) {
  const bool valid = slot.magic == magic &&
                     slot.check == checksum(magic, slot.data, sizeof slot.data);
  if (valid) {
    memcpy(&out, slot.data, sizeof out);
  }
  slot.magic = 0;
  return valid;
}

template <typename T>
void storeSlot(Slot<T>& slot, uint32_t magic, const T& value) {
  slot.magic = 0;  // invalid while the payload changes
  memcpy(slot.data, &value, sizeof value);
  slot.check = checksum(magic, slot.data, sizeof slot.data);
  slot.magic = magic;
}

bool isFaultException(mbed_error_status_t status) {
  const int code = MBED_GET_ERROR_CODE(status);
  return code == MBED_ERROR_CODE_HARDFAULT_EXCEPTION ||
         code == MBED_ERROR_CODE_MEMMANAGE_EXCEPTION ||
         code == MBED_ERROR_CODE_BUSFAULT_EXCEPTION ||
         code == MBED_ERROR_CODE_USAGEFAULT_EXCEPTION;
}

}  // namespace

// Overrides Mbed OS's weak hook, called for every reported error just before
// a fatal one halts the CPU. Fault context, maybe an ISR: plain stores only.
extern "C" void mbed_error_hook(const mbed_error_ctx* context) {
  ResetHistory::FaultCapture fault;
  fault.status = static_cast<uint32_t>(context->error_status);
  fault.address = context->error_address;
  if (isFaultException(context->error_status) && context->error_value != 0) {
    // mbed_fault_handler passes its register dump as the error value.
    fault.address =
        reinterpret_cast<const mbed_fault_context_t*>(context->error_value)
            ->PC_reg;
  }
  if (breadcrumbSlot.magic == kBreadcrumbMagic) {
    ResetHistory::Breadcrumb current;
    memcpy(&current, breadcrumbSlot.data, sizeof current);
    fault.feeds = current.feeds;
  }
  storeSlot(faultSlot, kFaultMagic, fault);
}

namespace RetainedRam {

void take(ResetHistory::Breadcrumb& breadcrumb, bool& hasBreadcrumb,
          ResetHistory::FaultCapture& fault, bool& hasFault) {
  hasBreadcrumb = takeSlot(breadcrumbSlot, kBreadcrumbMagic, breadcrumb);
  hasFault = takeSlot(faultSlot, kFaultMagic, fault);
}

void store(const ResetHistory::Breadcrumb& breadcrumb) {
  storeSlot(breadcrumbSlot, kBreadcrumbMagic, breadcrumb);
}

uint16_t heapKiB() {
  const uint32_t used = static_cast<uint32_t>(mallinfo().uordblks);
  const uint32_t kib = (used + 1023) / 1024;
  return kib > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(kib);
}

}  // namespace RetainedRam
