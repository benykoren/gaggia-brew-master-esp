#include "temp_sensor.h"
#include "config.h"
#include <Arduino.h>
#include <string.h>

static HardwareSerial SensorSerial(1); // UART1

// 200ms is the proven-safe value from bench testing. An earlier attempt to
// tighten this to 100ms caused real, repeated timeouts in normal operation
// (the module doesn't always reply as fast as the handful of sniffer tests
// suggested) - do not lower this again without measuring actual worst-case
// reply latency over a long run first.
static const uint32_t READ_TIMEOUT_MS = 200;

void tempSensorInit() {
  SensorSerial.begin(SENSOR_BAUD, SERIAL_8N1, PIN_SENSOR_RX, PIN_SENSOR_TX);
}

TempSensorStatus tempSensorRead(float &outC) {
  while (SensorSerial.available())
    SensorSerial.read(); // drop anything stale before asking

  SensorSerial.print("AT+T\r\n");
  SensorSerial.flush();

  char buf[48];
  size_t len = 0;
  uint32_t start = millis();
  bool sawError = false;
  while (millis() - start < READ_TIMEOUT_MS) {
    while (SensorSerial.available() && len < sizeof(buf) - 1) {
      buf[len++] = (char)SensorSerial.read();
    }
    buf[len] = '\0';
    if (strstr(buf, "OK") != nullptr) break;
    if (strstr(buf, "ERROR") != nullptr) {
      sawError = true;
      break;
    }
  }
  buf[len] = '\0';

  float t;
  if (!sawError && sscanf(buf, "+T=%f", &t) == 1) {
    outC = t;
    return TempSensorStatus::OK;
  }
  return sawError ? TempSensorStatus::ERROR_REPLY : TempSensorStatus::TIMEOUT;
}
