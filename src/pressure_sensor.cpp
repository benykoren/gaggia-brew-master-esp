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
  // Pulled down, not a plain floating input: an unconnected ADC1 pin floats
  // and reads noisy mid-scale voltages, which the calibration formula below
  // converts to plausible-looking bar values right around
  // PUMP_MAX_SAFETY_BAR - exactly the failure mode that let sensor noise
  // intermittently force the pump off during Milestone A's dimmer bench
  // test, before the transducer is even wired in (see main.cpp's
  // pressureClosedLoopActive-gated safety-ceiling check). With the pulldown,
  // an unwired sensor reads ~0mV -> bar ~= -2.0 -> below the -0.5
  // plausibility floor below -> deterministically OUT_OF_RANGE instead of a
  // plausible-but-wrong reading.
  pinMode(PIN_PRESSURE_ADC, INPUT_PULLDOWN);
  analogReadResolution(12); // 0-4095, matches the ESP32-S3 ADC's native width
}

PressureSensorStatus pressureSensorRead(float &outBar) {
  // analogReadMilliVolts() uses the chip's factory eFuse ADC calibration and
  // its nonlinearity correction near the rails, instead of a hand-rolled
  // raw-count-to-mV linear scale - improves accuracy for the bench
  // calibration step (Milestone B).
  uint32_t mv = analogReadMilliVolts(PIN_PRESSURE_ADC);

  float bar = ((float)mv - PRESSURE_SENSOR_ZERO_MV) / PRESSURE_SENSOR_MV_PER_BAR;

  if (bar < -0.5f || bar > PRESSURE_PLAUSIBLE_MAX_BAR) {
    outBar = 0.0f;
    return PressureSensorStatus::OUT_OF_RANGE;
  }
  if (bar < 0.0f) bar = 0.0f; // small negative noise around true zero
  outBar = bar;
  return PressureSensorStatus::OK;
}
