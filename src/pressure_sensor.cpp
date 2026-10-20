#include "pressure_sensor.h"

#include <Arduino.h>

#include "config.h"

// A reading is only plausible up to 1.5x the transducer's rated full scale
// (PRESSURE_SENSOR_MAX_BAR) - its output clips near full scale, so a raw
// reading well past that almost certainly means a wiring fault / sensor
// floating high, not a genuine spike. (Was derived from
// PUMP_MAX_SAFETY_BAR before the installed sensor turned out to be a 5-bar
// part, which is below that ceiling.)
static const float PRESSURE_PLAUSIBLE_MAX_BAR = PRESSURE_SENSOR_MAX_BAR * 1.5f;

static volatile uint32_t lastMv = 0;

uint32_t pressureSensorLastMv() { return lastMv; }

void pressureSensorInit() {
#if PRESSURE_SENSOR_IS_16BAR
  // Plain input, no internal pull: the external 4.3k/10k divider (config.h)
  // already holds GPIO1 to GND through 10k, and the internal pull-down
  // (~45k) in parallel would skew the divider ratio by ~7%. A disconnected
  // sensor still reads ~0 mV through the 10k -> bar ~= -2 -> below the
  // -0.5 plausibility floor -> deterministically OUT_OF_RANGE.
  pinMode(PIN_PRESSURE_ADC, INPUT);
#else
  // 0-5 bar part wired straight to GPIO1: pull-down so an unwired pin reads
  // ~0 mV (-> OUT_OF_RANGE) instead of floating to plausible-looking values.
  pinMode(PIN_PRESSURE_ADC, INPUT_PULLDOWN);
#endif
  analogReadResolution(12); // 0-4095, matches the ESP32-S3 ADC's native width
}

PressureSensorStatus pressureSensorRead(float &outBar) {
  // analogReadMilliVolts() uses the chip's factory eFuse ADC calibration and
  // its nonlinearity correction near the rails, instead of a hand-rolled
  // raw-count-to-mV linear scale - improves accuracy for the bench
  // calibration step (Milestone B).
  // Oversampled (2026-10-20): single reads jumped ~170-215 mV at rest
  // (+-0.15 bar at 175 mV/bar). Averaging a burst of reads cuts that before
  // the EMA in main.cpp, so the EMA can stay light (fast for the safety
  // ceiling) and pressure_mv in /status is steady enough to calibrate from.
  // 16 reads take well under 1 ms.
  const int PRESSURE_OVERSAMPLE = 16;
  uint32_t sum = 0;
  for (int i = 0; i < PRESSURE_OVERSAMPLE; i++) sum += analogReadMilliVolts(PIN_PRESSURE_ADC);
  uint32_t mv = (sum + PRESSURE_OVERSAMPLE / 2) / PRESSURE_OVERSAMPLE;
  lastMv = mv;

  float bar = ((float)mv - PRESSURE_SENSOR_ZERO_MV) / PRESSURE_SENSOR_MV_PER_BAR;

  if (bar < -0.5f || bar > PRESSURE_PLAUSIBLE_MAX_BAR) {
    outBar = 0.0f;
    return PressureSensorStatus::OUT_OF_RANGE;
  }
  if (bar < 0.0f) bar = 0.0f; // small negative noise around true zero
  // Anything above rated full scale is "at least full scale", not a real
  // number - report the ceiling rather than an extrapolated value.
  if (bar > PRESSURE_SENSOR_MAX_BAR) bar = PRESSURE_SENSOR_MAX_BAR;
  outBar = bar;
  return PressureSensorStatus::OK;
}
