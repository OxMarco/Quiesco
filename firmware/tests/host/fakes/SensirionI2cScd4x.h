// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include "Wire.h"
#define SCD41_I2C_ADDR_62 0x62
extern bool sensorReady;
struct SensirionI2CTxFrame {
 static SensirionI2CTxFrame createWithUInt16Command(uint16_t,uint8_t*,unsigned){return {};}
};
struct SensirionI2CCommunication {
 static int sendFrame(uint8_t,SensirionI2CTxFrame&,TwoWire&){return 0;}
};
class SensirionI2cScd4x {
 public:
 void begin(TwoWire&,uint8_t){}
 int getSerialNumber(uint64_t& n){n=123;return 0;}
 int getDataReadyStatus(bool& b){b=sensorReady;return 0;}
 int readMeasurement(uint16_t& c,float& t,float& h){c=450;t=20;h=50;return 0;}
 int setAmbientPressure(uint32_t){return 0;}
 int getAutomaticSelfCalibrationEnabled(uint16_t& e){e=0;return 0;}
 int setAutomaticSelfCalibrationEnabled(uint16_t){return 0;}
 int persistSettings(){return 0;}
 int performForcedRecalibration(uint16_t,uint16_t& r){r=0x8000;return 0;}
};
