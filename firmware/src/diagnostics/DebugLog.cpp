// SPDX-License-Identifier: GPL-3.0-only
#include "DebugLog.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

#include "BuildConfig.h"
#include "Trace.h"
#include "../model/Config.h"
#include "../model/FaultStatus.h"
#include "../model/Reading.h"
#include "../services/SampleRecord.h"
#include "../ui/UiModel.h"

namespace DebugLog {

namespace {

// One output line built from printf formats, then written in one call. Text
// past the buffer is cut, never overflowed. The image already links newlib's
// full vsnprintf (the renderer uses it), so %f and %llu cost no extra flash.
class Line {
 public:
  Line() { text_[0] = '\0'; }

  __attribute__((format(printf, 2, 3))) void add(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int length =
        vsnprintf(text_ + used_, sizeof text_ - used_, format, args);
    va_end(args);
    if (length > 0) {
      used_ += static_cast<size_t>(length);
      if (used_ > sizeof text_ - 1) {
        used_ = sizeof text_ - 1;
      }
    }
  }

  const char* text() const { return text_; }
  void print() const { Serial.println(text_); }

 private:
  char text_[512];
  size_t used_ = 0;
};

void addReading(Line& line, const char* label, float value, bool valid,
                int decimals) {
  if (valid) {
    line.add("%s%.*f", label, decimals, static_cast<double>(value));
  } else {
    line.add("%sn/a", label);
  }
}

const char* faultName(const DeviceFault& fault, bool succeeded) {
  if (!fault.present) {
    return "missing";
  }
  if (fault.timedOut) {
    return "timeout";
  }
  return succeeded ? "ok" : "read-error";
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
  Line line;
  line.add("Quiesco firmware=%s serial=%s reset=%s", firmwareVersion,
           serialNumber, resetReason);
  line.print();
#if QUIESCO_TRACE
  // Marks each boot in the flash ring: a reset while unattended shows here.
  Line kept;
  kept.add("t=%lu boot firmware=%s reset=%s",
           static_cast<unsigned long>(millis()), firmwareVersion, resetReason);
  Trace::keep(kept.text());
#endif
}

void cycleComplete(unsigned long cycleNumber, const Reading& reading,
                   const FaultStatus& faults, ScreenId screen, bool charging,
                   bool rendered, uint32_t logSequence) {
  if (!kDebugEnabled) {
    return;
  }
  constexpr uint32_t kBmeReadings =
      VALID_TEMPERATURE | VALID_HUMIDITY | VALID_PRESSURE;
  const auto healthy = [](const DeviceFault& fault) {
    return fault.present && fault.consecutiveFailures == 0;
  };
  // Whether each device delivered this cycle, in the order both lines list
  // them.
  const struct {
    const char* name;
    const DeviceFault* fault;
    bool succeeded;
  } devices[] = {
      {"bme", &faults.bme280,
       (reading.valid & kBmeReadings) == kBmeReadings},
      {"veml", &faults.veml7700, (reading.valid & VALID_LIGHT) != 0},
      {"scd", &faults.scd41, (reading.valid & VALID_CO2) != 0},
      {"mic", &faults.microphone, (reading.valid & VALID_NOISE) != 0},
      {"batt", &faults.battery, (reading.valid & VALID_BATTERY) != 0},
      {"display", &faults.display, healthy(faults.display)},
      {"flash", &faults.flash, healthy(faults.flash)},
      {"ble", &faults.ble, healthy(faults.ble)},
  };

  Line line;
  line.add("cycle=%lu", cycleNumber);
  addReading(line, " T=", reading.temperatureC,
             reading.valid & VALID_TEMPERATURE, 2);
  addReading(line, "C RH=", reading.humidityPct,
             reading.valid & VALID_HUMIDITY, 1);
  addReading(line, "% P=", reading.pressurePa / 100.0f,
             reading.valid & VALID_PRESSURE, 1);
  addReading(line, "hPa CO2=", reading.co2Ppm, reading.valid & VALID_CO2, 0);
  addReading(line, "ppm lux=", reading.lux, reading.valid & VALID_LIGHT, 1);
  addReading(line, " noise=", reading.noiseDb, reading.valid & VALID_NOISE, 1);
  line.add("dB");
  addReading(line, " battery=", reading.batteryV,
             reading.valid & VALID_BATTERY, 2);
  line.add("%s screen=%s draw=%s log=#%lu", charging ? "V charging" : "V",
           screenName(screen), rendered ? "yes" : "skip",
           static_cast<unsigned long>(logSequence));
  for (const auto& device : devices) {
    line.add(" %s=%s failures=%u", device.name,
             faultName(*device.fault, device.succeeded),
             static_cast<unsigned>(device.fault->consecutiveFailures));
  }
  line.print();

#if QUIESCO_TRACE
  // A compact copy for the flash ring. Per device: o ok, m missing,
  // t timeout, e read-error, then the consecutive-failure count. The
  // readings themselves are in the sample log (#log).
  const long lux10 = (reading.valid & VALID_LIGHT)
                         ? static_cast<long>(reading.lux * 10.0f + 0.5f)
                         : -1;
  Line kept;
  kept.add("t=%lu cycle=%lu log=#%lu lux10=%ld screen=%s draw=%s",
           static_cast<unsigned long>(millis()), cycleNumber,
           static_cast<unsigned long>(logSequence), lux10,
           screenName(screen), rendered ? "yes" : "skip");
  for (const auto& device : devices) {
    kept.add(" %s=%c%u", device.name,
             faultCode(*device.fault, device.succeeded),
             static_cast<unsigned>(device.fault->consecutiveFailures));
  }
  Trace::keep(kept.text());
#endif
}

void event(const char* what, long value) {
  if (!kDebugEnabled) {
    return;
  }
  Line line;
  line.add("t=%lu %s %ld", static_cast<unsigned long>(millis()), what, value);
#if QUIESCO_TRACE
  // Slow steps come every cycle (persist, refresh) and would halve the
  // ring's reach; the trace keeps the refresh time and resets instead.
  if (strncmp(what, "step slow", 9) != 0) {
    Trace::record(line.text());  // printed and kept in the flash ring
    return;
  }
#endif
  line.print();
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
  Line device;
  device.add(
      "info firmware=%s serial=%s boot=%lu reset=%s uptime_s=%lu log=#%lu "
      "bonds=%u usb=%s%s",
      info.firmwareVersion, info.serialNumber,
      static_cast<unsigned long>(info.bootCount), info.resetReason,
      static_cast<unsigned long>(info.uptimeMs / 1000),
      static_cast<unsigned long>(info.lastSequence),
      static_cast<unsigned>(info.bondCount), info.usbPowered ? "yes" : "no",
      kBatteryFitted ? "" : " battery=none");
  device.print();

  Line settings;
  settings.add(
      "info name=%s interval_s=%lu screen=%u unit=%c offsets noise=%.2f "
      "temp=%.2f rh=%.2f frc_target=%u frc_correction=%ld",
      config.deviceName,
      static_cast<unsigned long>(config.measurementIntervalSeconds),
      static_cast<unsigned>(config.displayScreen),
      config.temperatureUnit == TEMPERATURE_UNIT_FAHRENHEIT ? 'F' : 'C',
      static_cast<double>(config.noiseOffsetDb),
      static_cast<double>(config.tempOffsetC),
      static_cast<double>(config.humidityOffsetRh),
      static_cast<unsigned>(config.frcTargetPpm),
      static_cast<long>(config.frcCorrectionPpm));
  settings.print();
}

void logDumpBegin(const char* serialNumber, uint32_t lastSequence) {
  if (!kDebugEnabled) {
    return;
  }
  Line line;
  line.add("# begin quiesco-log v1 serial=%s last=%lu", serialNumber,
           static_cast<unsigned long>(lastSequence));
  line.print();
  Serial.println(
      "sequence,boot,epoch_s,uptime_ms,valid,temperature_c,humidity_pct,"
      "pressure_pa,co2_ppm,lux,noise_db,battery_v,scd_temperature_c,"
      "scd_humidity_pct");
}

void logDumpRecord(const SampleRecord& record) {
  if (!kDebugEnabled) {
    return;
  }
  Line line;
  line.add("%lu,%lu,%lu,%llu,%u", static_cast<unsigned long>(record.sequence),
           static_cast<unsigned long>(record.bootCount),
           static_cast<unsigned long>(record.epochSeconds),
           static_cast<unsigned long long>(record.monotonicMs),
           static_cast<unsigned>(record.validFlags));
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
    if (record.validFlags & field.bit) {
      line.add(",%.*f", field.decimals, static_cast<double>(field.value));
    } else {
      line.add(",");
    }
  }
  if (record.flags & kSampleScdRht) {
    line.add(",%.2f,%.2f",
             static_cast<double>(record.scdTemperatureCenti / 100.0f),
             static_cast<double>(record.scdHumidityCenti / 100.0f));
  } else {
    line.add(",,");
  }
  line.print();
}

void logDumpEnd(uint32_t records, bool complete) {
  if (!kDebugEnabled) {
    return;
  }
  Line line;
  line.add("# end %srecords=%lu", complete ? "" : "incomplete ",
           static_cast<unsigned long>(records));
  line.print();
}

void stress(uint32_t operations, uint32_t failures, uint32_t lastSequence,
            bool done) {
  if (!kDebugEnabled) {
    return;
  }
  Line line;
  line.add("stress %sops=%lu failed=%lu log=#%lu", done ? "done " : "",
           static_cast<unsigned long>(operations),
           static_cast<unsigned long>(failures),
           static_cast<unsigned long>(lastSequence));
  line.print();
}

}  // namespace DebugLog
