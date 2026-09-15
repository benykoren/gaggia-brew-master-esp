#include "dimmer.h"

#include <Arduino.h>
#include <esp_timer.h>

#include "config.h"

// Stored as tenths-of-a-percent (0-1000), NOT a float, and deliberately so:
// onZeroCross() below runs in genuine GPIO interrupt context, and on this
// chip the floating-point coprocessor's register state is only saved/
// restored across FreeRTOS task context switches, not interrupt entry/exit.
// Any float math reachable from an ISR risks corrupting whatever task's FPU
// state was live at the moment of interruption, surfacing as an
// intermittent "Guru Meditation Error: Coprocessor exception" panic -
// confirmed on real hardware (2026-09-15): the crash only started once the
// dimmer began receiving real zero-cross pulses from live mains for the
// first time (all earlier bench testing never exercised this path). Every
// value the ISR touches, directly or via a function it calls, must stay
// integer-only from here down.
static volatile int32_t targetPercentX10 = 0; // 0-1000 = 0.0%-100.0%
static esp_timer_handle_t fireTimer = nullptr;
static volatile int64_t lastZcUs = 0;

// Temporary bring-up diagnostic (2026-09-15) - counts accepted zero-cross
// interrupts, so main.cpp can expose a rate (crossings/sec) via /status and
// confirm whether the ESP32 is actually seeing a real, continuous ~100Hz
// signal from the dimmer's Z-C output, independent of whether the module
// has a status LED at all or whether the TRIAC is actually firing. Plain
// integer increment - ISR-safe, no float involved.
static volatile uint32_t zcCount = 0;

// Converts tenths-of-a-percent (0-1000) into a firing delay after the
// zero-cross, in microseconds - pure integer arithmetic, callable from ISR
// context (see the ISR-safety note above). Linear percent-to-delay mapping
// (not the more accurate cosine/RMS-power curve real dimmers use) - a
// reasonable first pass per HARDWARE_ROADMAP.md item 8; the pressure PID
// trims around whatever curve this produces during closed-loop tuning
// (bring-up Task 12), the same "tune against real hardware" pattern already
// used for temperature PID.
static uint32_t percentX10ToDelayUs(int32_t percentX10) {
  if (percentX10 >= 1000) return DIMMER_MIN_FIRING_DELAY_US;
  int64_t delay = (int64_t)DIMMER_AC_HALF_CYCLE_US * (1000 - percentX10) / 1000;
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
  zcCount++;

  int32_t percentX10 = targetPercentX10;
  if (percentX10 <= 0) return; // stay off - no timer scheduled at all

  esp_timer_stop(fireTimer);
  esp_timer_start_once(fireTimer, percentX10ToDelayUs(percentX10));
}

void dimmerInit() {
  pinMode(PIN_DIMMER_GATE, OUTPUT);
  digitalWrite(PIN_DIMMER_GATE, LOW);
  // Opto-isolated zero-cross detector outputs are commonly open-collector -
  // pull up so a floating/idle line reads a deterministic HIGH instead of
  // risking spurious interrupt triggers from a floating input.
  pinMode(PIN_DIMMER_ZC, INPUT_PULLUP);

  esp_timer_create_args_t timerArgs = {};
  timerArgs.callback = &fireGate;
  timerArgs.name = "dimmer_fire";
  esp_timer_create(&timerArgs, &fireTimer);

  attachInterruptArg(digitalPinToInterrupt(PIN_DIMMER_ZC),
                      (void (*)(void *))onZeroCross, nullptr, RISING);
}

// Float math here is safe - this always runs in normal FreeRTOS task
// context (called from controlTick()/applyShotStagePumpOutput() etc.),
// never from the ISR. The float-to-integer conversion below is the one
// place a percent value crosses from "safe to use float" task context into
// the ISR-visible integer representation.
void dimmerSetPowerPercent(float percent) {
  if (percent < 0.0f) percent = 0.0f;
  if (percent > 100.0f) percent = 100.0f;
  int32_t x10 = (int32_t)(percent * 10.0f + 0.5f); // round to nearest tenth
  targetPercentX10 = x10;
  if (percent <= 0.0f) {
    // A safety cutoff to 0% must not still be followed by one
    // already-scheduled gate pulse up to ~10ms later from a timer armed
    // just before the cutoff was requested - cancel it outright. Return
    // code ignored, same as onZeroCross()'s own esp_timer_stop() call
    // above: it's a no-op if the timer isn't currently armed.
    esp_timer_stop(fireTimer);
  }
}

float dimmerGetPowerPercent() { return (float)targetPercentX10 / 10.0f; }

// Temporary bring-up diagnostic - see zcCount's declaration comment above.
uint32_t dimmerGetZcCount() { return zcCount; }
