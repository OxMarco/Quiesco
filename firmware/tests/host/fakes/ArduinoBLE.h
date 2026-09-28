// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Executable, shared-state GATT fake. Copies of BLECharacteristic refer to
// the same value, matching ArduinoBLE's local-characteristic handles.
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <stdint.h>

enum BLEProperty : uint8_t { BLERead = 2, BLEWriteWithoutResponse = 4,
                            BLEWrite = 8, BLENotify = 16 };
enum BLEPermission : uint16_t { BLEEncryption = 1 << 9 };
enum Pairable { NO = 0, YES = 1, ONCE = 2 };
enum BLECharacteristicEvent : uint8_t {
  BLESubscribed = 0, BLEUnsubscribed = 1, BLERead_ = 2, BLEWritten = 3,
};
enum BLEDeviceEvent : uint8_t { BLEConnected = 0, BLEDisconnected = 1 };
class BLEDevice {
 public:
  explicit BLEDevice(int id = 0) : id_(id) {}
  operator bool() const { return id_ != 0; }
  bool operator==(const BLEDevice& other) const { return id_ == other.id_; }
 private:
  int id_;
};
class BLECharacteristic;
using BLECharacteristicEventHandler = void (*)(BLEDevice, BLECharacteristic);
using BLEDeviceEventHandler = void (*)(BLEDevice);
namespace FakeBle {
struct Value {
  int capacity = 0;
  bool fixed = false;
  bool subscribed = false;
  std::vector<uint8_t> bytes;
  BLECharacteristicEventHandler handler = nullptr;
};
inline std::map<std::string, std::shared_ptr<Value>>& values() {
  static std::map<std::string, std::shared_ptr<Value>> entries;
  return entries;
}
inline std::vector<std::function<void()>>& events() {
  static std::vector<std::function<void()>> pending;
  return pending;
}
inline std::array<BLEDeviceEventHandler, 2>& handlers() {
  static std::array<BLEDeviceEventHandler, 2> entries = {};
  return entries;
}
inline BLEDevice& peer() { static BLEDevice device; return device; }
inline void connect(int id) {
  peer() = BLEDevice(id);
  if (handlers()[BLEConnected]) handlers()[BLEConnected](peer());
}
inline void disconnect() {
  const BLEDevice old = peer();
  for (auto& entry : values()) entry.second->subscribed = false;
  if (handlers()[BLEDisconnected]) handlers()[BLEDisconnected](old);
  peer() = BLEDevice();
}
inline std::vector<uint8_t> read(const char* uuid) { return values().at(uuid)->bytes; }
}
class BLECharacteristic {
 public:
  BLECharacteristic(const char* uuid, uint16_t, int size, bool fixed)
      : value_(new FakeBle::Value) {
    value_->capacity = size; value_->fixed = fixed;
    value_->bytes.resize(fixed ? size : 0);
    FakeBle::values()[uuid] = value_;
  }
  BLECharacteristic(const char* uuid, uint16_t properties, const char* value)
      : BLECharacteristic(uuid, properties, std::strlen(value), true) {
    writeValue(reinterpret_cast<const uint8_t*>(value), std::strlen(value));
  }
  explicit BLECharacteristic(std::shared_ptr<FakeBle::Value> value) : value_(value) {}
  int valueLength() const { return static_cast<int>(value_->bytes.size()); }
  int readValue(uint8_t* buffer, int length) {
    length = std::min(length, valueLength());
    if (length) std::memcpy(buffer, value_->bytes.data(), length);
    return length;
  }
  int writeValue(const uint8_t* value, int length) {
    length = std::min(length, value_->capacity);
    value_->bytes.assign(value, value + length);
    if (value_->fixed) value_->bytes.resize(value_->capacity);
    return valueLength();
  }
  int writeValue(const uint8_t* value, size_t length) { return writeValue(value, static_cast<int>(length)); }
  bool subscribed() { return value_->subscribed; }
  void setEventHandler(int, BLECharacteristicEventHandler handler) { value_->handler = handler; }
 private:
  std::shared_ptr<FakeBle::Value> value_;
};
namespace FakeBle {
inline void write(const char* uuid, const uint8_t* bytes, int length) {
  auto state = values().at(uuid);
  BLECharacteristic characteristic(state);
  characteristic.writeValue(bytes, length);
  if (state->handler) state->handler(peer(), characteristic);
}
}
class BLEService {
 public:
  explicit BLEService(const char*) {}
  void addCharacteristic(BLECharacteristic&) {}
};
class BLEClass {
 public:
  int begin() { return 1; }
  void poll() {
    while (!FakeBle::events().empty()) {
      auto event = FakeBle::events().front();
      FakeBle::events().erase(FakeBle::events().begin());
      event();
    }
  }
  bool connected() { poll(); return bool(FakeBle::peer()); }
  int advertise() { return 1; }
  void stopAdvertise() {}
  void setLocalName(const char*) {}
  void setDeviceName(const char*) {}
  void setAdvertisedService(const BLEService&) {}
  void addService(BLEService&) {}
  void setPairable(uint8_t) {}
  void setEventHandler(BLEDeviceEvent event, BLEDeviceEventHandler handler) {
    FakeBle::handlers()[event] = handler;
  }
};
extern BLEClass BLE;
