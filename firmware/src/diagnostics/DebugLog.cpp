// SPDX-License-Identifier: GPL-3.0-only
#include "DebugLog.h"

#include <Arduino.h>

#include "BuildConfig.h"
#include "Trace.h"
#include "../model/Config.h"
#include "../model/FaultStatus.h"
#include "../model/Reading.h"
#include "../services/SampleRecord.h"
#include "../ui/UiModel.h"

namespace DebugLog {

namespace {

void printReading(const char* label, float value, bool valid, int decimals) {
  Serial.print(label);
  if (valid) {
    Serial.print(value, decimals);
  } else {
    Serial.print("n/a");
  }
}

void printFault(const char* label, const DeviceFault& fault, bool succeeded) {
  Serial.print(label);
  if (!fault.present) {
    Serial.print("missing");
  } else if (fault.timedOut) {
    Serial.print("timeout");
  } else if (succeeded) {
    Serial.print("ok");
  } else {
    Serial.print("read-error");
  }
  Serial.print(" failures=");
  Serial.print(static_cast<unsigned int>(fault.consecutiveFailures));
}

#if QUIESCO_TRACE
char faultCode(const DeviceFault& fault, bool succeeded) {
  if (!fault.present) {
    return 'm';
  }
  if (fault.timedOut) {
    return 't';
  }
  return succeeded ? 'o' : 'e';
}

unsigned faultCount(const DeviceFault& fault) {
  return fault.consecutiveFailures;
}
#endif

const char* screenName(ScreenId screen) {
  switch (screen) {
    case ScreenId::kFace:
      return "face";
    case ScreenId::kLedger:
      return "ledger";
    case ScreenId::kBento:
      return "bento";
    case ScreenId::kBattery:
      return "battery";
    case ScreenId::kUnavailable:
      return "unavailable";
    case ScreenId::kPairing:
      return "pairing";
  }
  return "?";
}

}  // namespace

void begin(const char* firmwareVersion, const char* serialNumber,
           const char* resetReason) {
  if (!kDebugEnabled) {
    return;
  }
  Serial.begin(115200);
  Serial.print("Quiesco firmware=");
  Serial.print(firmwareVersion);
  Serial.print(" serial=");
  Serial.print(serialNumber);
  Serial.print(" reset=");
  Serial.println(resetReason);
#if QUIESCO_TRACE
  // Marks each boot in the flash ring: a reset while unattended shows here.
  char line[112];
  snprintf(line, sizeof line, "t=%lu boot firmware=%s reset=%s",
           static_cast<unsigned long>(millis()), firmwareVersion, resetReason);
  Trace::keep(line);
#endif
}

void cycleComplete(unsigned long cycleNumber, const Reading& reading,
                   const FaultStatus& faults, ScreenId screen, bool charging,
                   bool rendered, uint32_t logSequence) {
  if (!kDebugEnabled) {
    return;
  }
  Serial.print("cycle=");
  Serial.print(cycleNumber);
  printReading(" T=", reading.temperatureC,
               reading.valid & VALID_TEMPERATURE, 2);
  printReading("C RH=", reading.humidityPct, reading.valid & VALID_HUMIDITY, 1);
  printReading("% P=", reading.pressurePa / 100.0f,
               reading.valid & VALID_PRESSURE, 1);
  printReading("hPa CO2=", reading.co2Ppm, reading.valid & VALID_CO2, 0);
  printReading("ppm lux=", reading.lux, reading.valid & VALID_LIGHT, 1);
  printReading(" noise=", reading.noiseDb, reading.valid & VALID_NOISE, 1);
  Serial.print("dB");
  printReading(" battery=", reading.batteryV, reading.valid & VALID_BATTERY,
               2);
  Serial.print(charging ? "V charging" : "V");
  Serial.print(" screen=");
  Serial.print(screenName(screen));
  Serial.print(rendered ? " draw=yes" : " draw=skip");
  Serial.print(" log=#");
  Serial.print(static_cast<unsigned long>(logSequence));

  constexpr uint32_t kBmeReadings =
      VALID_TEMPERATURE | VALID_HUMIDITY | VALID_PRESSURE;
  printFault(" bme=", faults.bme280,
             (reading.valid & kBmeReadings) == kBmeReadings);
  printFault(" veml=", faults.veml7700, reading.valid & VALID_LIGHT);
  printFault(" scd=", faults.scd41, reading.valid & VALID_CO2);
  printFault(" mic=", faults.microphone, reading.valid & VALID_NOISE);
  printFault(" batt=", faults.battery, reading.valid & VALID_BATTERY);
  printFault(" display=", faults.display,
             faults.display.present &&
                 faults.display.consecutiveFailures == 0);
  printFault(" flash=", faults.flash,
             faults.flash.present && faults.flash.consecutiveFailures == 0);
  printFault(" ble=", faults.ble,
             faults.ble.present && faults.ble.consecutiveFailures == 0);
  Serial.println();

#if QUIESCO_TRACE
  // A compact copy for the flash ring. Per device: o ok, m missing,
  // t timeout, e read-error, then the consecutive-failure count. The
  // readings themselves are in the sample log (#log).
  const bool bmeOk = (reading.valid & kBmeReadings) == kBmeReadings;
  const bool displayOk =
      faults.display.present && faults.display.consecutiveFailures == 0;
  const bool flashOk =
      faults.flash.present && faults.flash.consecutiveFailures == 0;
  const bool bleOk = faults.ble.present && faults.ble.consecutiveFailures == 0;
  const long lux10 = (reading.valid & VALID_LIGHT)
                         ? static_cast<long>(reading.lux * 10.0f + 0.5f)
                         : -1;
  char line[200];
  snprintf(line, sizeof line,
           "t=%lu cycle=%lu log=#%lu lux10=%ld screen=%s draw=%s "
           "bme=%c%u veml=%c%u scd=%c%u mic=%c%u batt=%c%u display=%c%u "
           "flash=%c%u ble=%c%u",
           static_cast<unsigned long>(millis()), cycleNumber,
           static_cast<unsigned long>(logSequence), lux10,
           screenName(screen), rendered ? "yes" : "skip",
           faultCode(faults.bme280, bmeOk), faultCount(faults.bme280),
           faultCode(faults.veml7700, reading.valid & VALID_LIGHT),
           faultCount(faults.veml7700),
           faultCode(faults.scd41, reading.valid & VALID_CO2),
           faultCount(faults.scd41),
           faultCode(faults.microphone, reading.valid & VALID_NOISE),
           faultCount(faults.microphone),
           faultCode(faults.battery, reading.valid & VALID_BATTERY),
           faultCount(faults.battery), faultCode(faults.display, displayOk),
           faultCount(faults.display), faultCode(faults.flash, flashOk),
           faultCount(faults.flash), faultCode(faults.ble, bleOk),
           faultCount(faults.ble));
  Trace::keep(line);
#endif
}

void event(const char* what, long value) {
  if (!kDebugEnabled) {
    return;
  }
#if QUIESCO_TRACE
  char line[96];
  snprintf(line, sizeof line, "t=%lu %s %ld",
           static_cast<unsigned long>(millis()), what, value);
  // Slow steps come every cycle (persist, refresh) and would halve the
  // ring's reach; the trace keeps the refresh time and resets instead.
  if (strncmp(what, "step slow", 9) == 0) {
    Serial.println(line);
  } else {
    Trace::record(line);  // printed and kept in the flash ring
  }
  return;
#endif
  Serial.print("t=");
  Serial.print(millis());
  Serial.print(' ');
  Serial.print(what);
  Serial.print(' ');
  if (value < 0) {
    Serial.print('-');
  }
  Serial.println(static_cast<unsigned long>(value < 0 ? -value : value));
}

char pollCommand() {
  if (!kDebugEnabled || Serial.available() <= 0) {
    return 0;
  }
  const int value = Serial.read();
  return value > ' ' && value < 0x7F ? static_cast<char>(value) : 0;
}

void help() {
  if (!kDebugEnabled) {
    return;
  }
  Serial.println(
      "commands: i info, m measure now, d dump log as CSV, "
      "w 45 s flash-write stress (unplug during it), "
      "t print the trace ring (trace builds), ? help");
}

void info(const Info& info, const Config& config) {
  if (!kDebugEnabled) {
    return;
  }
  Serial.print("info firmware=");
  Serial.print(info.firmwareVersion);
  Serial.print(" serial=");
  Serial.print(info.serialNumber);
  Serial.print(" boot=");
  Serial.print(static_cast<unsigned long>(info.bootCount));
  Serial.print(" reset=");
  Serial.print(info.resetReason);
  Serial.print(" uptime_s=");
  Serial.print(static_cast<unsigned long>(info.uptimeMs / 1000));
  Serial.print(" log=#");
  Serial.print(static_cast<unsigned long>(info.lastSequence));
  Serial.print(" bonds=");
  Serial.print(static_cast<unsigned int>(info.bondCount));
  Serial.print(info.usbPowered ? " usb=yes" : " usb=no");
  if (!kBatteryFitted) {
    Serial.print(" battery=none");
  }
  Serial.println();
  Serial.print("info name=");
  Serial.print(config.deviceName);
  Serial.print(" interval_s=");
  Serial.print(static_cast<unsigned long>(config.measurementIntervalSeconds));
  Serial.print(" screen=");
  Serial.print(static_cast<unsigned int>(config.displayScreen));
  Serial.print(config.temperatureUnit == TEMPERATURE_UNIT_FAHRENHEIT ? " unit=F"
                                                                     : " unit=C");
  Serial.print(" offsets noise=");
  Serial.print(config.noiseOffsetDb, 2);
  Serial.print(" temp=");
  Serial.print(config.tempOffsetC, 2);
  Serial.print(" rh=");
  Serial.print(config.humidityOffsetRh, 2);
  Serial.print(" frc_target=");
  Serial.print(static_cast<unsigned int>(config.frcTargetPpm));
  Serial.print(" frc_correction=");
  Serial.print(static_cast<long>(config.frcCorrectionPpm));
  Serial.println();
}

void logDumpBegin(const char* serialNumber, uint32_t lastSequence) {
  if (!kDebugEnabled) {
    return;
  }
  Serial.print("# begin quiesco-log v1 serial=");
  Serial.print(serialNumber);
  Serial.print(" last=");
  Serial.println(static_cast<unsigned long>(lastSequence));
  Serial.println(
      "sequence,boot,epoch_s,uptime_ms,valid,temperature_c,humidity_pct,"
      "pressure_pa,co2_ppm,lux,noise_db,battery_v,scd_temperature_c,"
      "scd_humidity_pct");
}

void logDumpRecord(const SampleRecord& record) {
  if (!kDebugEnabled) {
    return;
  }
  Serial.print(static_cast<unsigned long>(record.sequence));
  Serial.print(',');
  Serial.print(static_cast<unsigned long>(record.bootCount));
  Serial.print(',');
  Serial.print(static_cast<unsigned long>(record.epochSeconds));
  Serial.print(',');
  Serial.print(static_cast<unsigned long long>(record.monotonicMs));
  Serial.print(',');
  Serial.print(static_cast<unsigned int>(record.validFlags));
  // Invalid fields print empty: they are not real zeros.
  const struct {
    uint32_t bit;
    float value;
    int decimals;
  } fields[] = {
      {VALID_TEMPERATURE, record.temperatureC, 2},
      {VALID_HUMIDITY, record.humidityPct, 2},
      {VALID_PRESSURE, record.pressurePa, 0},
      {VALID_CO2, record.co2Ppm, 0},
      {VALID_LIGHT, record.lux, 1},
      {VALID_NOISE, record.noiseDb, 1},
      {VALID_BATTERY, record.batteryV, 3},
  };
  for (const auto& field : fields) {
    Serial.print(',');
    if (record.validFlags & field.bit) {
      Serial.print(field.value, field.decimals);
    }
  }
  Serial.print(',');
  if (record.flags & kSampleScdRht) {
    Serial.print(record.scdTemperatureCenti / 100.0f, 2);
    Serial.print(',');
    Serial.print(record.scdHumidityCenti / 100.0f, 2);
  } else {
    Serial.print(',');
  }
  Serial.println();
}

void logDumpEnd(uint32_t records, bool complete) {
  if (!kDebugEnabled) {
    return;
  }
  Serial.print(complete ? "# end records=" : "# end incomplete records=");
  Serial.println(static_cast<unsigned long>(records));
}

void stress(uint32_t operations, uint32_t failures, uint32_t lastSequence,
            bool done) {
  if (!kDebugEnabled) {
    return;
  }
  Serial.print(done ? "stress done ops=" : "stress ops=");
  Serial.print(static_cast<unsigned long>(operations));
  Serial.print(" failed=");
  Serial.print(static_cast<unsigned long>(failures));
  Serial.print(" log=#");
  Serial.println(static_cast<unsigned long>(lastSequence));
}

}  // namespace DebugLog
