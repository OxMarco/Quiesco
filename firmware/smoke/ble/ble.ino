// SPDX-License-Identifier: GPL-3.0-only
// ArduinoBLE spike on Seeeduino Mbed 2.9.3. Answers,
// on real hardware with nRF Connect as the peer:
//  1. Does ArduinoBLE 2.1.0 compile/link on this core, and at what size?
//  2. Advertise a 128-bit service UUID + local name; do both fit (adv vs
//     scan response)? Is the device visible and connectable?
//  3. Polled written() on a R/W characteristic; notify characteristic.
//  4. Largest notify the stack accepts: writeValue() return per payload size
//     (ArduinoBLE has no MTU getter; the phone app reports its MTU instead).
//  5. Notify throughput and backpressure behaviour at a delay(1) poll cadence.
//  6. Loop rate with BLE.poll() every ~1 ms (idle-current proxy; measure
//     current separately).
//  7. Rename + re-advertise while running (device name change path).
//  8. Disconnect/reconnect robustness.
// Record the outcomes in SOFTWARE.md and flip ArduinoBLE's dependencies.lock
// status before relying on drivers/BleConfig.

#include <ArduinoBLE.h>

#if !defined(ARDUINO_SEEED_XIAO_NRF52840_PLUS) && \
    !defined(ARDUINO_SEEED_XIAO_NRF52840_SENSE_PLUS)
#error "Select board: XIAO nRF52840 (Sense) Plus - Seeed nRF52 mbed core"
#endif

BLEService service("7A1EFFFF-8E6F-4A7A-AE32-515549455343");
BLECharacteristic echoCharacteristic("7A1EFFFE-8E6F-4A7A-AE32-515549455343",
                                     BLERead | BLEWrite, 20, false);
BLECharacteristic notifyCharacteristic("7A1EFFFD-8E6F-4A7A-AE32-515549455343",
                                       BLENotify, 180, false);

bool wasConnected = false;
bool renamed = false;
bool sizeProbeDone = false;
bool throughputDone = false;
uint32_t loopCount = 0;
uint32_t loopWindowStartMs = 0;

void setup() {
  Serial.begin(115200);
  const uint32_t serialWaitUntil = millis() + 3000;
  while (!Serial && millis() < serialWaitUntil) {
  }
  Serial.println("== Quiesco ArduinoBLE spike ==");

  if (!BLE.begin()) {
    Serial.println("FAIL: BLE.begin()");
    while (true) {
    }
  }
  BLE.setLocalName("Quiesco-Spike");
  BLE.setAdvertisedService(service);
  service.addCharacteristic(echoCharacteristic);
  service.addCharacteristic(notifyCharacteristic);
  BLE.addService(service);
  const uint8_t initial[4] = {'i', 'n', 'i', 't'};
  echoCharacteristic.writeValue(initial, sizeof initial);
  Serial.print("advertise: ");
  Serial.println(BLE.advertise() ? "OK" : "FAIL");
  Serial.println("Check in nRF Connect: name AND 128-bit UUID visible?");
  loopWindowStartMs = millis();
}

void probeNotifySizes() {
  // The write succeeds into the local value regardless; what matters is the
  // return code once a central subscribed — a 0 marks the size the stack
  // refused to notify at the negotiated MTU.
  static uint8_t payload[180];
  const uint16_t sizes[] = {20, 48, 60, 120, 180};
  Serial.println("notify size probe (subscribe first):");
  for (uint16_t size : sizes) {
    for (uint16_t i = 0; i < size; i++) {
      payload[i] = static_cast<uint8_t>(i);
    }
    const int result = notifyCharacteristic.writeValue(payload, size);
    Serial.print("  ");
    Serial.print(size);
    Serial.print(" bytes -> ");
    Serial.println(result);
    delay(50);
  }
}

void measureThroughput() {
  static uint8_t payload[20];
  uint32_t sent = 0, rejected = 0;
  const uint32_t untilMs = millis() + 5000;
  while (millis() < untilMs) {
    payload[0] = static_cast<uint8_t>(sent);
    if (notifyCharacteristic.writeValue(payload, sizeof payload)) {
      sent++;
    } else {
      rejected++;  // backpressure: stack buffer full
    }
    BLE.poll();
    delay(1);
  }
  Serial.print("throughput over 5 s at delay(1): sent=");
  Serial.print(sent);
  Serial.print(" rejected=");
  Serial.println(rejected);
}

void loop() {
  BLE.poll();
  loopCount++;
  const uint32_t nowMs = millis();
  if (nowMs - loopWindowStartMs >= 10000) {
    Serial.print("loop rate: ");
    Serial.print(loopCount / 10);
    Serial.println("/s (poll + delay(1))");
    loopCount = 0;
    loopWindowStartMs = nowMs;
  }

  const bool connected = BLE.connected();
  if (connected && !wasConnected) {
    Serial.println("central connected");
    sizeProbeDone = false;
    throughputDone = false;
  }
  if (!connected && wasConnected) {
    Serial.println("central disconnected; advertising should self-resume:");
    Serial.println(BLE.advertise() ? "  re-advertise OK" : "  re-advertise FAIL");
  }
  wasConnected = connected;

  if (connected && echoCharacteristic.written()) {
    uint8_t buffer[20];
    const int length = echoCharacteristic.readValue(buffer, sizeof buffer);
    Serial.print("echo write of ");
    Serial.print(length);
    Serial.println(" bytes; echoed back");
    echoCharacteristic.writeValue(buffer, length);
    // A written echo kicks off the notify experiments exactly once each.
    if (!sizeProbeDone && notifyCharacteristic.subscribed()) {
      probeNotifySizes();
      sizeProbeDone = true;
    } else if (!throughputDone && notifyCharacteristic.subscribed()) {
      measureThroughput();
      throughputDone = true;
    }
  }

  // Rename while advertising after 30 s: the device-name change path.
  if (!renamed && nowMs > 30000 && !connected) {
    BLE.stopAdvertise();
    BLE.setLocalName("Quiesco-Renamed");
    Serial.print("rename + re-advertise: ");
    Serial.println(BLE.advertise() ? "OK" : "FAIL");
    renamed = true;
  }

  delay(1);
}
