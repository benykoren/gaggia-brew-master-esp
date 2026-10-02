#include "dimmer.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/gptimer.h>
#include <esp_timer.h>

#include "config.h"

// ISR-SAFETY: onZeroCross() and onGateAlarm() run in genuine interrupt
// context, where this chip does not save FPU state - any float math
// reachable from them can corrupt a task's FPU registers ("Guru Meditation
// Error: Coprocessor exception", confirmed on real hardware 2026-09-15).
// Everything the ISRs touch below is integer-only. Floats appear only in
// task-context functions (dimmerSetPowerPercent*, dimmerTick, getters).

static volatile int32_t requestedX10 = 0; // 0-1000 = 0.0-100.0 %
static volatile int32_t appliedX10 = 0;   // what the ISR actually fires
static volatile int64_t lastZcUs = 0;
static volatile int32_t debounceUs = DIMMER_AC_HALF_CYCLE_US / 2;
static volatile uint32_t zcCount = 0;
static volatile uint32_t firedCycles = 0;
static volatile bool psmResetRequested = false;
static gptimer_handle_t gateTimer = nullptr;

// Pulse-skip state - ISR-owned. A "cycle" is two consecutive accepted
// crossings; the fire/skip decision is made on the first and held for the
// second, so a fired cycle always covers the half-cycle the pump's diode
// conducts on. Accumulator starts half full so a low duty fires its first
// stroke promptly instead of after up to 1/duty cycles.
static int32_t psmAccumulator = 500;
static bool psmFireThisCycle = false;
static bool psmSecondHalf = false;

// Mains health - task-owned (dimmerTick).
static uint32_t windowStartMs = 0;
static uint32_t windowStartZc = 0;
static float mainsHz = DIMMER_MAINS_HZ_DEFAULT;
static float lastWindowHz = DIMMER_MAINS_HZ_DEFAULT;
static MainsState mainsState = MainsState::ABSENT;

static inline void gateLow() { gpio_set_level((gpio_num_t)PIN_DIMMER_GATE, 0); }
static inline void gateHigh() { gpio_set_level((gpio_num_t)PIN_DIMMER_GATE, 1); }

static bool IRAM_ATTR onGateAlarm(gptimer_handle_t, const gptimer_alarm_event_data_t *, void *) {
  gateLow();
  return false;
}

static void IRAM_ATTR onZeroCross(void *) {
  int64_t now = esp_timer_get_time();
  if (now - lastZcUs < debounceUs) return; // ZC modules chatter around the crossing
  lastZcUs = now;
  zcCount++;

  if (psmResetRequested) {
    psmResetRequested = false;
    psmAccumulator = 500;
    psmFireThisCycle = false;
    psmSecondHalf = false;
  }

  int32_t percentX10 = appliedX10;
  if (percentX10 <= 0 || gateTimer == nullptr) {
    psmAccumulator = 500;
    psmFireThisCycle = false;
    psmSecondHalf = false;
    gateLow();
    return;
  }

  if (!psmSecondHalf) {
    psmAccumulator += percentX10;
    if (psmAccumulator >= 1000) {
      psmAccumulator -= 1000;
      psmFireThisCycle = true;
      firedCycles++;
    } else {
      psmFireThisCycle = false;
    }
  }
  psmSecondHalf = !psmSecondHalf;

  if (!psmFireThisCycle) {
    gateLow();
    return;
  }
  gateHigh();
  uint64_t count = 0;
  gptimer_get_raw_count(gateTimer, &count);
  gptimer_alarm_config_t alarm = {};
  alarm.alarm_count = count + DIMMER_GATE_HOLD_US; // 1 MHz timer: counts = us
  gptimer_set_alarm_action(gateTimer, &alarm);
}

void dimmerInit() {
  pinMode(PIN_DIMMER_GATE, OUTPUT);
  gateLow();
  // Opto-isolated ZC outputs are commonly open-collector - pull up so an
  // idle line reads a deterministic HIGH, not a floating input.
  pinMode(PIN_DIMMER_ZC, INPUT_PULLUP);

  // Fail-safe: if the timer can't be set up, gateTimer stays null and the
  // ISR never raises the gate - pump off, logged, rather than a boot loop.
  gptimer_handle_t t = nullptr;
  gptimer_config_t cfg = {};
  cfg.clk_src = GPTIMER_CLK_SRC_DEFAULT;
  cfg.direction = GPTIMER_COUNT_UP;
  cfg.resolution_hz = 1000000;
  gptimer_event_callbacks_t cbs = {};
  cbs.on_alarm = onGateAlarm;
  if (gptimer_new_timer(&cfg, &t) == ESP_OK && gptimer_register_event_callbacks(t, &cbs, nullptr) == ESP_OK &&
      gptimer_enable(t) == ESP_OK && gptimer_start(t) == ESP_OK) {
    gateTimer = t;
  } else {
    Serial.println("dimmer: gptimer setup FAILED - pump disabled");
  }

  windowStartMs = millis();
  windowStartZc = 0;
  attachInterruptArg(digitalPinToInterrupt(PIN_DIMMER_ZC), (void (*)(void *))onZeroCross, nullptr, RISING);
}

static int32_t toX10(float percent) {
  if (!(percent > 0.0f)) return 0; // also NaN
  if (percent > 100.0f) percent = 100.0f;
  return (int32_t)(percent * 10.0f + 0.5f);
}

void dimmerSetPowerPercent(float percent) {
  int32_t x10 = toX10(percent);
  requestedX10 = x10;
  if (x10 < appliedX10) appliedX10 = x10; // decreases are instant
  if (x10 == 0) gateLow();                // a cut must not wait for the alarm
}

void dimmerSetPowerPercentImmediate(float percent) {
  int32_t x10 = toX10(percent);
  requestedX10 = x10;
  appliedX10 = x10;
  if (x10 == 0) gateLow();
}

void dimmerTick(uint32_t dtMs) {
  int32_t req = requestedX10, app = appliedX10;
  if (app < req) {
    int32_t step = (int32_t)((DIMMER_SLEW_PCT_PER_S * 10UL * dtMs) / 1000UL);
    if (step < 1) step = 1;
    app += step;
    if (app > req) app = req;
    appliedX10 = app;
  }

  uint32_t nowMs = millis();
  if (nowMs - windowStartMs >= 1000) {
    uint32_t zc = zcCount;
    float secs = (nowMs - windowStartMs) / 1000.0f;
    lastWindowHz = ((zc - windowStartZc) / 2.0f) / secs;
    windowStartZc = zc;
    windowStartMs = nowMs;
    if (lastWindowHz >= DIMMER_MAINS_HZ_MIN && lastWindowHz <= DIMMER_MAINS_HZ_MAX) {
      mainsHz = lastWindowHz;
      debounceUs = (int32_t)(250000.0f / mainsHz); // half of one half-cycle
    }
  }

  MainsState prev = mainsState;
  int64_t sinceZcUs = esp_timer_get_time() - lastZcUs;
  if (lastZcUs == 0 || sinceZcUs > (int64_t)DIMMER_MAINS_ABSENT_MS * 1000) {
    mainsState = MainsState::ABSENT;
  } else if (lastWindowHz < DIMMER_MAINS_HZ_MIN || lastWindowHz > DIMMER_MAINS_HZ_MAX) {
    mainsState = MainsState::UNSTABLE;
  } else {
    mainsState = MainsState::OK;
  }
  if (mainsState == MainsState::ABSENT && prev != MainsState::ABSENT) {
    psmResetRequested = true; // clean pattern when power returns
    gateLow();
  }
}

float dimmerGetPowerPercent() { return (float)appliedX10 / 10.0f; }
uint32_t dimmerGetZcCount() { return zcCount; }
uint32_t dimmerGetFiredCycles() { return firedCycles; }
MainsState dimmerGetMainsState() { return mainsState; }
float dimmerGetMainsHz() { return mainsHz; }

const char *mainsStateName(MainsState s) {
  switch (s) {
    case MainsState::OK: return "ok";
    case MainsState::UNSTABLE: return "unstable";
    default: return "absent";
  }
}
