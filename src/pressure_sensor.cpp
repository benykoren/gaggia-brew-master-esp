#include "pressure_sensor.h"

#include <Arduino.h>

#include "config.h"

// A reading is only plausible up to 1.5x the safety ceiling - real bar
// values on this machine should never approach the transducer's full range
// (the safety valve caps physical pressure well below that), so a raw
// reading this high almost certainly means a disconnected sensor floating
// high, not a genuine spike.
static const float PRESSURE_PLAUSIBLE_MAX_BAR = PUMP_MAX_SAFETY_BAR * 1.5f;

void pressureSensorInit() {
  pinMode(PIN_PRESSURE_ADC, INPUT);
  analogReadResolution(12); // 0-4095, matches the ESP32-S3 ADC's native width
}

PressureSensorStatus pressureSensorRead(float &outBar) {
  int raw = analogRead(PIN_PRESSURE_ADC);
  float mv = (raw / 4095.0f) * 3300.0f; // ESP32-S3 ADC1 full-scale is ~3.3V

  float bar = (mv - PRESSURE_SENSOR_ZERO_MV) / PRESSURE_SENSOR_MV_PER_BAR;

  if (bar < -0.5f || bar > PRESSURE_PLAUSIBLE_MAX_BAR) {
    outBar = 0.0f;
    return PressureSensorStatus::OUT_OF_RANGE;
  }
  if (bar < 0.0f) bar = 0.0f; // small negative noise around true zero
  outBar = bar;
  return PressureSensorStatus::OK;
}
