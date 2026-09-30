// SPDX-License-Identifier: GPL-3.0-only
#include <vector>

#include "doctest.h"

#include "board/BoardPins.h"
#include "board/PowerDomain.h"

namespace {

enum class Operation { kPinMode, kDigitalWrite };

struct Event {
  Operation operation;
  uint8_t pin;
  uint8_t value;
};

std::vector<Event> events;

}  // namespace

FakeSerial Serial;

void pinMode(uint8_t pin, uint8_t mode) {
  events.push_back({Operation::kPinMode, pin, mode});
}

void digitalWrite(uint8_t pin, uint8_t value) {
  events.push_back({Operation::kDigitalWrite, pin, value});
}

void delay(unsigned long) {}
uint32_t millis() { return 0; }

void FakeSerial::begin(unsigned long) {}
void FakeSerial::print(const char*) {}
void FakeSerial::print(unsigned long) {}
void FakeSerial::println(const char*) {}

TEST_CASE("power domain rail sequencing") {
  PowerDomain power;
  power.beginOff();

  REQUIRE_MESSAGE(events.size() >= 2, "beginOff should configure the rail");
  CHECK_MESSAGE((events[0].operation == Operation::kDigitalWrite &&
                 events[0].pin == BoardPins::kPeripheralRail &&
                 events[0].value == HIGH),
                "beginOff must preload the rail-off level first");
  CHECK_MESSAGE((events[1].operation == Operation::kPinMode &&
                 events[1].pin == BoardPins::kPeripheralRail &&
                 events[1].value == OUTPUT),
                "beginOff must make the preloaded rail pin an output second");
  CHECK_MESSAGE(!power.enabled(), "rail should report disabled after beginOff");

  events.clear();
  power.enable(500);
  CHECK_MESSAGE(power.enabled(), "rail should report enabled after enable");
  CHECK_MESSAGE(!power.ready(1499),
                "rail should not be ready before settle deadline");
  CHECK_MESSAGE(power.ready(1500), "rail should be ready at settle deadline");
  CHECK_MESSAGE((!events.empty() &&
                 events[0].operation == Operation::kDigitalWrite &&
                 events[0].pin == BoardPins::kPeripheralRail &&
                 events[0].value == LOW),
                "enable must drive the active-low rail on");

  power.disable();
  CHECK_MESSAGE(!power.enabled(), "rail should report disabled after disable");

  power.enable(500, PowerDomain::kBusSettleMs);
  CHECK_MESSAGE(!power.ready(500 + PowerDomain::kBusSettleMs - 1),
                "bus-only rail should not be ready before its settle deadline");
  CHECK_MESSAGE(power.ready(500 + PowerDomain::kBusSettleMs),
                "bus-only rail should be ready at its settle deadline");
  power.disable();
}
