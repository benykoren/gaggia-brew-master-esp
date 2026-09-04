#include "dimmer.h"

#include <Arduino.h>
#include <esp_timer.h>

#include "config.h"

static volatile float targetPercent = 0.0f;
static esp_timer_handle_t fireTimer = nullptr;
static volatile int64_t lastZcUs = 0;

// Converts 0-100% power into a firing delay after the zero-cross, in
// microseconds. Linear percent-to-delay mapping (not the more accurate
// cosine/RMS-power curve real dimmers use) - a reasonable first pass per
// HARDWARE_ROADMAP.md item 8; the pressure PID trims around whatever curve
// this produces during closed-loop tuning (bring-up Task 12), the same
// "tune against real hardware" pattern already used for temperature PID.
static uint32_t percentToDelayUs(float percent) {
  if (percent >= 100.0f) return DIMMER_MIN_FIRING_DELAY_US;
  float delay = DIMMER_AC_HALF_CYCLE_US * (1.0f - percent / 100.0f);
  if (delay < DIMMER_MIN_FIRING_DELAY_US) delay = DIMMER_MIN_FIRING_DELAY_US;
  return (uint32_t)delay;
}

static void IRAM_ATTR fireGate(void *arg) {
  digitalWrite(PIN_DIMMER_GATE, HIGH);
  ets_delay_us(DIMMER_GATE_PULSE_US);
  digitalWrite(PIN_DIMMER_GATE, LOW);
}

// Fires on every zero-cross transition. Debounced by elapsed time rather
// than a manual re-arm flag - a real zero-cross module can chatter multiple
// transitions around the same physical crossing; ignore anything closer
// than half a half-cycle to the last accepted crossing.
static void IRAM_ATTR onZeroCross(void *arg) {
  int64_t now = esp_timer_get_time();
  if (now - lastZcUs < (DIMMER_AC_HALF_CYCLE_US / 2)) return;
  lastZcUs = now;

  float percent = targetPercent;
  if (percent <= 0.0f) return; // stay off - no timer scheduled at all

  esp_timer_stop(fireTimer);
  esp_timer_start_once(fireTimer, percentToDelayUs(percent));
}

void dimmerInit() {
  pinMode(PIN_DIMMER_GATE, OUTPUT);
  digitalWrite(PIN_DIMMER_GATE, LOW);
  pinMode(PIN_DIMMER_ZC, INPUT);

  esp_timer_create_args_t timerArgs = {};
  timerArgs.callback = &fireGate;
  timerArgs.name = "dimmer_fire";
  esp_timer_create(&timerArgs, &fireTimer);

  attachInterruptArg(digitalPinToInterrupt(PIN_DIMMER_ZC),
                      (void (*)(void *))onZeroCross, nullptr, RISING);
}

void dimmerSetPowerPercent(float percent) {
  if (percent < 0.0f) percent = 0.0f;
  if (percent > 100.0f) percent = 100.0f;
  targetPercent = percent;
}

float dimmerGetPowerPercent() { return targetPercent; }
