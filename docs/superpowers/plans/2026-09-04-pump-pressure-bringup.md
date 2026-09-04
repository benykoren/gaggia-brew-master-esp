# Pump-Pressure Hardware Bring-Up Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the shelved NC pump relay with a phase-control AC dimmer
(HARDWARE_ROADMAP.md item 8) and a pressure transducer (item 7), delivering
plain on/off pump control plus closed-loop, multi-phase pressure-target
control integrated into the existing shot-stage state machine.

**Architecture:** Two new self-contained drivers (`pressure_sensor.cpp`,
`dimmer.cpp`) follow the existing `temp_sensor.cpp` pattern. `main.cpp`'s
`ShotStage` state machine gains a `PRESSURE_TARGET` type and a
`targetPressureBar` field; a second `PID_v1` instance closes the pressure
loop the same way the existing one closes the temperature loop. TRIAC gate
firing is the one piece of this that is genuinely timing-critical (mains
half-cycle ~10ms), so it runs from a zero-cross ISR + hardware one-shot
timer, outside the normal 50ms control-task cadence; everything else
(pressure read, fault detection, PID) joins the existing `controlTick()`.

**Tech Stack:** ESP32-S3 / Arduino framework / PlatformIO, `br3ttb/PID`,
`bblanchon/ArduinoJson` (existing deps, no new libraries needed — zero-cross
timing uses ESP-IDF's built-in `esp_timer`).

**Spec:** `docs/superpowers/specs/2026-09-04-pump-pressure-bringup-design.md`

## Global Constraints

- No mains-voltage work happens until the corresponding firmware has been
  bench-verified on low voltage first (`AGENTS.md` §6).
- No unit-test framework exists in this project (embedded, Arduino-tied,
  no `test/` dir) — every code task's verification step is a clean
  `pio run` build; physical behavior is verified in the three bench
  milestone tasks (10-12), matching how every prior hardware feature in
  this codebase was verified (real-hardware bench tests, not automated
  tests).
- Build command: `& "C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\pio.exe" run -e esp32-s3-devkitc-1`
- ADC1 only for the pressure transducer (GPIO1) — ADC2 is WiFi-contaminated.
- The old NC relay (`PIN_PUMP`, `setPumpRelay()`) is removed entirely, not
  kept as a fallback — explicit decision, spec §3.
- `PUMP_MAX_SAFETY_BAR = 12.0` always wins over PID/profile output, exactly
  like `activeMaxSafety` does for temperature.
- Backward compatible: profiles saved before this change (no pressure
  fields) must keep behaving exactly as today — plain 100%/0% duty, no
  pressure control.

---

## Task 1: config.h — new pins, constants, ShotPhase::PRESSURE

**Files:**
- Modify: `include/config.h:98-110` (pin definitions block)
- Modify: `include/config.h:380` (`ShotPhase` enum)

**Interfaces:**
- Produces: `PIN_DIMMER_GATE`, `PIN_DIMMER_ZC`, `PIN_PRESSURE_ADC`,
  `DIMMER_AC_HALF_CYCLE_US`, `DIMMER_MIN_FIRING_DELAY_US`,
  `DIMMER_GATE_PULSE_US`, `PRESSURE_SENSOR_ZERO_MV`,
  `PRESSURE_SENSOR_MV_PER_BAR`, `PRESSURE_FAULT_WINDOW`,
  `PRESSURE_FAULT_MIN_SAMPLES`, `PRESSURE_FAULT_RATE_THRESHOLD`,
  `PUMP_MAX_SAFETY_BAR`, `PUMP_PRESSURE_KP_DEFAULT`,
  `PUMP_PRESSURE_KI_DEFAULT`, `PUMP_PRESSURE_KD_DEFAULT`,
  `PRESSURE_RAMP_BAR_DEFAULT`, `PRESSURE_RAMP_MS_DEFAULT`,
  `PRESSURE_DECLINE_BAR_DEFAULT`, `PRESSURE_DECLINE_MS_DEFAULT`,
  `PRESSURE_PROFILE_STAGE_COUNT`, `ShotPhase::PRESSURE` — all consumed by
  later tasks. **Does not remove `PIN_PUMP`/`PIN_PUMP_ACTIVE_LEVEL` yet**
  (Task 4 does, once nothing references them any more) — this task is
  purely additive so it stays independently buildable.

- [ ] **Step 1: Add the new pin/constant block**

Insert immediately after the existing pin-definitions comment block, right
before the current `#define PIN_PUMP 5` line (`include/config.h:98-110`
stays untouched for now):

```cpp
// ============================================================================
// Pump pressure control (HARDWARE_ROADMAP.md items 7/8, 2026-09-04) - AC
// phase-control dimmer + pressure transducer. GPIO5 (freed once the old NC
// relay is removed - see Task 4) becomes the TRIAC gate-fire output; GPIO6
// is the dimmer's zero-cross detect input; GPIO1 (ADC1_CH0) is the pressure
// transducer's analog input. ADC1 only - ADC2 shares hardware with WiFi and
// becomes unavailable when WiFi is active (same rule already followed for
// temp sensing).
// ============================================================================
#define PIN_DIMMER_GATE 5
#define PIN_DIMMER_ZC 6
#define PIN_PRESSURE_ADC 1

// Mains half-cycle at 50Hz is ~10ms (Israel/Europe mains) - change to 8333
// if this machine is ever run on a 60Hz supply. DIMMER_MIN_FIRING_DELAY_US
// is how close to the zero-cross the TRIAC is allowed to fire (0 risks
// firing into switching noise right at the crossing); DIMMER_GATE_PULSE_US
// is the minimum gate-trigger pulse width most TRIAC modules need.
#define DIMMER_AC_HALF_CYCLE_US 10000
#define DIMMER_MIN_FIRING_DELAY_US 300
#define DIMMER_GATE_PULSE_US 100

// Pressure transducer calibration - linear mapping from ADC millivolts to
// bar: bar = (mv - PRESSURE_SENSOR_ZERO_MV) / PRESSURE_SENSOR_MV_PER_BAR.
// These are PLACEHOLDERS until the actual transducer is bench-calibrated
// against a known reference (0 bar with the circuit open to atmosphere,
// pump off) - see bring-up Task 11. Defaults below assume a common
// 0.5-4.5V-output/12-16bar transducer wired through a 2-resistor divider
// that halves it to 0.25-2.25V at the ADC pin.
#define PRESSURE_SENSOR_ZERO_MV 250.0f
#define PRESSURE_SENSOR_MV_PER_BAR 125.0f // (2250-250)/16 bar

// Same rolling error-rate fault model as the temp sensor (see
// SENSOR_FAULT_WINDOW above) - a separate window since pressure and temp
// fail independently.
#define PRESSURE_FAULT_WINDOW 20
#define PRESSURE_FAULT_MIN_SAMPLES 5
#define PRESSURE_FAULT_RATE_THRESHOLD 0.5f

// Hard safety ceiling - comfortably under the machine's 16-bar safety valve
// rating (docs/oem-manuals/hydraulic-schematic-SAI0103.pdf), above the
// ~9 bar working target. Forces the dimmer to 0% immediately if exceeded or
// if the pressure sensor is faulted, independent of PID/profile output -
// same "cutoff wins over everything" rule as activeMaxSafety for
// temperature. Fixed, not Web-UI-configurable (mirrors BREW_MAX_SAFETY,
// not the configurable STEAM_MAX_SAFETY - no tuning need identified yet).
#define PUMP_MAX_SAFETY_BAR 12.0

// Pressure PID defaults - live-tunable from the Web UI (Pump Pressure
// tuning card, Task 8), same pattern as Brew/Steam Kp/Ki/Kd. Untuned
// starting guesses; expect real iteration against the real machine
// (bring-up Task 12), the same way brew-temperature PID tuning did.
#define PUMP_PRESSURE_KP_DEFAULT 8.0
#define PUMP_PRESSURE_KI_DEFAULT 0.5
#define PUMP_PRESSURE_KD_DEFAULT 0.0

// Per-profile pressure stages (HARDWARE_ROADMAP.md item 8) - an optional
// "ramp/hold" stage at a target bar for a fixed duration, then an optional
// "decline" stage at a lower target for its own fixed duration,
// approximating a declining-pressure curve near the end of a shot. After
// these, the shot falls into the normal open-ended EXTRACTION stage,
// holding the last pressure target (or 100% duty if pressure control isn't
// enabled for this profile - unchanged, backward-compatible behavior).
#define PRESSURE_RAMP_BAR_DEFAULT 9.0
#define PRESSURE_RAMP_MS_DEFAULT 20000
#define PRESSURE_DECLINE_BAR_DEFAULT 6.0
#define PRESSURE_DECLINE_MS_DEFAULT 8000
// Fixed count of profile-defined pressure stages (ramp + decline) - used
// only to size main.cpp's activeShotStages array.
#define PRESSURE_PROFILE_STAGE_COUNT 2

```

- [ ] **Step 2: Add `PRESSURE` to the `ShotPhase` enum**

Change `include/config.h:380` from:

```cpp
enum class ShotPhase { NONE, PREINFUSION_ON, PREINFUSION_OFF, EXTRACTION };
```

to:

```cpp
enum class ShotPhase { NONE, PREINFUSION_ON, PREINFUSION_OFF, PRESSURE, EXTRACTION };
```

- [ ] **Step 3: Build**

Run: `& "C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\pio.exe" run -e esp32-s3-devkitc-1`
Expected: clean build, no errors (nothing references the new defines yet,
so this is purely additive).

- [ ] **Step 4: Commit**

```bash
git add include/config.h
git commit -m "Add pump-pressure pin/constant definitions (dimmer + transducer)"
```

---

## Task 2: pressure_sensor driver

**Files:**
- Create: `include/pressure_sensor.h`
- Create: `src/pressure_sensor.cpp`

**Interfaces:**
- Consumes: `PIN_PRESSURE_ADC`, `PRESSURE_SENSOR_ZERO_MV`,
  `PRESSURE_SENSOR_MV_PER_BAR`, `PUMP_MAX_SAFETY_BAR` (all from Task 1's
  `config.h`).
- Produces: `enum class PressureSensorStatus { OK, OUT_OF_RANGE }`,
  `void pressureSensorInit()`, `PressureSensorStatus pressureSensorRead(float &outBar)`
  — consumed by Task 4 (`main.cpp`).

- [ ] **Step 1: Write `include/pressure_sensor.h`**

```cpp
#pragma once

// Analog pressure transducer driver (HARDWARE_ROADMAP.md item 7).
//
// Reads PIN_PRESSURE_ADC (ADC1 - see config.h for why not ADC2) and converts
// the raw millivolt reading to bar via a linear calibration
// (PRESSURE_SENSOR_ZERO_MV/PRESSURE_SENSOR_MV_PER_BAR in config.h) that must
// be bench-calibrated against the actual transducer part - see the pump-
// pressure bring-up design spec, bring-up Task 11.

enum class PressureSensorStatus { OK, OUT_OF_RANGE };

void pressureSensorInit();

// Reads the ADC, converts to bar, and writes it to `outBar`. Returns
// OUT_OF_RANGE (outBar left at 0) if the raw reading is implausible (below
// the zero-mv floor or well above what a healthy reading on this machine
// could ever be) - almost always a disconnected/failed sensor rather than a
// real reading, since the safety ceiling (PUMP_MAX_SAFETY_BAR) is well
// inside the transducer's actual range.
PressureSensorStatus pressureSensorRead(float &outBar);
```

- [ ] **Step 2: Write `src/pressure_sensor.cpp`**

```cpp
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
```

- [ ] **Step 3: Build**

Run: `& "C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\pio.exe" run -e esp32-s3-devkitc-1`
Expected: clean build (new files compile standalone; nothing calls them yet).

- [ ] **Step 4: Commit**

```bash
git add include/pressure_sensor.h src/pressure_sensor.cpp
git commit -m "Add pressure transducer driver"
```

---

## Task 3: dimmer driver

**Files:**
- Create: `include/dimmer.h`
- Create: `src/dimmer.cpp`

**Interfaces:**
- Consumes: `PIN_DIMMER_GATE`, `PIN_DIMMER_ZC`, `DIMMER_AC_HALF_CYCLE_US`,
  `DIMMER_MIN_FIRING_DELAY_US`, `DIMMER_GATE_PULSE_US` (Task 1's `config.h`).
- Produces: `void dimmerInit()`, `void dimmerSetPowerPercent(float percent)`,
  `float dimmerGetPowerPercent()` — consumed by Task 4 (`main.cpp`).

- [ ] **Step 1: Write `include/dimmer.h`**

```cpp
#pragma once

// AC phase-control dimmer driver (HARDWARE_ROADMAP.md item 8) - zero-cross
// detection + TRIAC gate firing via a hardware one-shot timer, so firing
// timing survives WiFi/BT scheduling jitter (a plain delayMicroseconds() in
// the zero-cross ISR does not - see config.h).

// Configures PIN_DIMMER_ZC as an interrupt input and PIN_DIMMER_GATE as an
// output, and creates the internal esp_timer used for gate firing. Call
// once from setup().
void dimmerInit();

// Sets the target power level (0-100), clamped to that range. 0 = TRIAC
// never fires (pump off); 100 = fires as close to the zero-cross as
// DIMMER_MIN_FIRING_DELAY_US allows (full pass-through). Safe to call from
// any task - the target is a plain volatile float, not behind main.cpp's
// stateMutex, because it's also read from the zero-cross ISR, where taking
// a FreeRTOS mutex isn't safe.
void dimmerSetPowerPercent(float percent);

// Returns the last percent passed to dimmerSetPowerPercent(), for bumpless
// stage transitions and /status reporting.
float dimmerGetPowerPercent();
```

- [ ] **Step 2: Write `src/dimmer.cpp`**

```cpp
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
```

- [ ] **Step 3: Build**

Run: `& "C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\pio.exe" run -e esp32-s3-devkitc-1`
Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add include/dimmer.h src/dimmer.cpp
git commit -m "Add AC phase-control dimmer driver"
```

---

## Task 4: main.cpp — remove relay, wire dimmer + pressure PID into the shot-stage machine

This is the pivotal task: it removes the old relay entirely and rebuilds
`ShotStage` around the dimmer + pressure PID. It also extracts a shared
fault-window helper (used by both temp and pressure) as a targeted, in-scope
DRY improvement, since this task is already duplicating that exact pattern.

**Files:**
- Modify: `src/main.cpp` (multiple sections, detailed below)
- Modify: `include/config.h:98-110` (remove `PIN_PUMP`/`PIN_PUMP_ACTIVE_LEVEL`)

**Interfaces:**
- Consumes: `dimmerInit()`, `dimmerSetPowerPercent()`, `dimmerGetPowerPercent()`
  (Task 3); `pressureSensorInit()`, `pressureSensorRead()`,
  `PressureSensorStatus` (Task 2); `ShotPhase::PRESSURE`,
  `PRESSURE_PROFILE_STAGE_COUNT`, `PUMP_PRESSURE_KP/KI/KD_DEFAULT`,
  `PUMP_MAX_SAFETY_BAR`, `PRESSURE_FAULT_*`, `PRESSURE_RAMP_BAR_DEFAULT`,
  `PRESSURE_RAMP_MS_DEFAULT`, `PRESSURE_DECLINE_BAR_DEFAULT`,
  `PRESSURE_DECLINE_MS_DEFAULT` (Task 1).
- Produces: `double pressureSetpoint, pressureInput, pressureOutput`,
  `double pressureKp, pressureKi, pressureKd`, `PID pressurePID`,
  `float currentPressure`, `bool pressureFault`,
  `bool activePressureEnabled`, `double activePressureRampBar`,
  `unsigned long activePressureRampMs`, `bool activePressureDeclineEnabled`,
  `double activePressureDeclineBar`, `unsigned long activePressureDeclineMs`,
  `float pressureHistory[TEMP_HISTORY_LEN]`, `int pressureHistoryHead`,
  `int pressureHistoryCount` — all consumed by Task 5 (profile wiring) and
  Task 6 (web status JSON).

- [ ] **Step 1: Add new includes**

At the top of `src/main.cpp`, add alongside the existing includes:

```cpp
#include "dimmer.h"
#include "pressure_sensor.h"
```

- [ ] **Step 2: Remove the old relay pin definitions from `config.h`**

Delete this whole block from `include/config.h:100-110`:

```cpp
// Pump relay (HARDWARE_ROADMAP.md item 4, revived 2026-08-30 - see AGENTS.md
// change log) - NC relay module, control side (GPIO5/VCC/GND) wired; the
// mains-side splice at the pump's own terminals (COM/NC) is not done yet.
// GPIO5 was free (next available after PIN_SSR=4, PIN_SENSOR_RX/TX=18/17).
// PIN_PUMP_ACTIVE_LEVEL=HIGH matches the relay module's trigger jumper (set
// to H) - bench-test this (relay should click on GPIO5 HIGH with nothing
// connected to COM/NO/NC) before ever wiring it to the pump. "Active"/
// energized means the NC contact is OPEN (interrupting the pump), not "pump
// on" - see setPumpRelay() in main.cpp.
#define PIN_PUMP 5
#define PIN_PUMP_ACTIVE_LEVEL HIGH
```

(The dimmer/pressure block Task 1 inserted right before it stays in place.)

- [ ] **Step 3: Rewrite the shot-stage section**

Replace the entire block from the `// Shot stage state machine` comment
through the end of `setPumpRelay()` (`src/main.cpp:496-539`) with:

```cpp
// ============================================================================
// Shot stage state machine (2026-08-23, generalized 2026-09-04 for pressure
// control) - a shot is a small ordered list of stages, advanced purely by
// millis() comparisons, same "declarative phase list" principle Gaggiuino/
// GaggiMate both use for their (much richer) pressure/flow profiles.
// currentShotPhase (config.h) is kept in sync from the active stage purely
// for external reporting (/status, MQTT) - nothing outside this file needs
// to know stages exist.
//
// PIN_DIMMER_GATE/PIN_DIMMER_ZC (HARDWARE_ROADMAP.md item 8) replace the old
// NC pump relay (item 4, removed 2026-09-04 after an unresolved brownout bug
// - see AGENTS.md change log). Unlike the relay, the dimmer has no passive
// pass-through state: it only conducts while firmware is actively firing the
// gate. This means the pump will not run at all if the ESP32 isn't running
// firmware, even with the Brew switch held - an explicitly accepted
// fail-off tradeoff (see the pump-pressure bring-up design spec, §3).
// ============================================================================
ShotPhase currentShotPhase = ShotPhase::NONE;

struct ShotStage {
  enum class Type { PUMP_ON, PUMP_OFF, PRESSURE_TARGET, EXTRACTION } type;
  unsigned long durationMs; // unused for EXTRACTION - runs until shot stop
  float targetPressureBar;  // only meaningful for PRESSURE_TARGET/EXTRACTION; 0 = no pressure target (plain duty)
};

// Longest possible sequence: PREINFUSION_PULSES_MAX on/off pairs, plus
// PRESSURE_PROFILE_STAGE_COUNT pressure stages (ramp + decline), plus the
// final EXTRACTION stage. Fixed-size, not std::vector - a shot starts often
// enough that a heap alloc/free every time is worth avoiding on an embedded
// target.
static ShotStage activeShotStages[PREINFUSION_PULSES_MAX * 2 + PRESSURE_PROFILE_STAGE_COUNT + 1];
static int activeStageCount = 0;
static int currentStageIndex = 0;
static unsigned long stageStartMillis = 0;

// Pressure PID (HARDWARE_ROADMAP.md item 8) - Input=measured bar,
// Output=0-100% dimmer power, Setpoint=whichever stage's targetPressureBar
// is active. Only driven while pressureClosedLoopActive is true (set by
// applyShotStagePumpOutput() below); MANUAL the rest of the time so it
// doesn't fight plain on/off duty. Live-tunable from the Web UI, same
// pattern as the Brew/Steam temperature PIDs.
double pressureSetpoint = 0, pressureInput = 0, pressureOutput = 0;
double pressureKp = PUMP_PRESSURE_KP_DEFAULT, pressureKi = PUMP_PRESSURE_KI_DEFAULT,
       pressureKd = PUMP_PRESSURE_KD_DEFAULT;
PID pressurePID(&pressureInput, &pressureOutput, &pressureSetpoint, pressureKp, pressureKi,
                pressureKd, DIRECT);
static bool pressureClosedLoopActive = false;

float currentPressure = 0.0;
bool pressureFault = false;

// Pressure history (for the Web UI pressure chart), sampled at the same
// cadence as tempHistory below.
float pressureHistory[TEMP_HISTORY_LEN] = {0};
int pressureHistoryHead = 0;
int pressureHistoryCount = 0;

// Per-profile pressure-stage settings (see profiles.cpp/Task 5) - plain
// globals with sensible defaults so buildShotStages() below works even
// before Task 5 wires them up to saved profiles.
bool activePressureEnabled = false;
double activePressureRampBar = PRESSURE_RAMP_BAR_DEFAULT;
unsigned long activePressureRampMs = PRESSURE_RAMP_MS_DEFAULT;
bool activePressureDeclineEnabled = false;
double activePressureDeclineBar = PRESSURE_DECLINE_BAR_DEFAULT;
unsigned long activePressureDeclineMs = PRESSURE_DECLINE_MS_DEFAULT;

// Drives the dimmer for whichever stage is now active. PUMP_ON/PUMP_OFF are
// plain duty (matches the old relay's on/off behavior exactly: PUMP_ON =
// pump running = 100%, PUMP_OFF = pump stopped = 0%). A PRESSURE_TARGET
// stage, or an EXTRACTION stage carrying a nonzero targetPressureBar (the
// shot's pressure profile continuing into the open-ended tail), hands
// control to the pressure PID. A plain EXTRACTION with no pressure target
// (profile has pressure control disabled) is full duty, identical to
// today's post-preinfusion behavior.
static void applyShotStagePumpOutput(const ShotStage &stage) {
  bool wantsPressureControl =
      (stage.type == ShotStage::Type::PRESSURE_TARGET) ||
      (stage.type == ShotStage::Type::EXTRACTION && stage.targetPressureBar > 0.0f);

  if (wantsPressureControl) {
    pressureSetpoint = stage.targetPressureBar;
    if (!pressureClosedLoopActive) {
      // Bumpless start: seed Output from wherever the dimmer already sits
      // (e.g. 100% coming out of a PUMP_ON pre-infusion pulse) instead of
      // snapping to 0 - same bumpless-reset principle used for temperature.
      pressureOutput = dimmerGetPowerPercent();
      pressurePID.SetMode(MANUAL);
      pressurePID.SetMode(AUTOMATIC);
    }
    pressureClosedLoopActive = true;
  } else {
    pressureClosedLoopActive = false;
    pressurePID.SetMode(MANUAL);
    dimmerSetPowerPercent(stage.type == ShotStage::Type::PUMP_OFF ? 0.0f : 100.0f);
  }
}

// Builds the stage sequence for the profile active at shot-start: pre-
// infusion pulses, then an optional pressure ramp/hold stage, then an
// optional pressure decline stage, then one open-ended EXTRACTION stage
// that inherits the last pressure target (0 = plain duty, if pressure
// control isn't enabled for this profile).
static void buildShotStages() {
  activeStageCount = 0;
  if (activePreinfusionEnabled && activePreinfusionPulses > 0) {
    for (int i = 0; i < activePreinfusionPulses; i++) {
      activeShotStages[activeStageCount++] = {ShotStage::Type::PUMP_ON, (unsigned long)activePreinfusionOnMs, 0.0f};
      if (i < activePreinfusionPulses - 1) {
        activeShotStages[activeStageCount++] = {ShotStage::Type::PUMP_OFF, (unsigned long)activePreinfusionOffMs, 0.0f};
      }
    }
  }

  float lastPressureTarget = 0.0f;
  if (activePressureEnabled) {
    activeShotStages[activeStageCount++] = {ShotStage::Type::PRESSURE_TARGET,
                                             activePressureRampMs, (float)activePressureRampBar};
    lastPressureTarget = (float)activePressureRampBar;
    if (activePressureDeclineEnabled) {
      activeShotStages[activeStageCount++] = {ShotStage::Type::PRESSURE_TARGET,
                                               activePressureDeclineMs, (float)activePressureDeclineBar};
      lastPressureTarget = (float)activePressureDeclineBar;
    }
  }

  activeShotStages[activeStageCount++] = {ShotStage::Type::EXTRACTION, 0, lastPressureTarget};
}

static ShotPhase phaseForStageType(ShotStage::Type t) {
  switch (t) {
    case ShotStage::Type::PUMP_ON: return ShotPhase::PREINFUSION_ON;
    case ShotStage::Type::PUMP_OFF: return ShotPhase::PREINFUSION_OFF;
    case ShotStage::Type::PRESSURE_TARGET: return ShotPhase::PRESSURE;
    default: return ShotPhase::EXTRACTION;
  }
}
```

- [ ] **Step 4: Update `tickShotStages()`, `startShot()`, `stopShot()`**

Replace `tickShotStages()` (`src/main.cpp:565-582` in the pre-change file)
with:

```cpp
// Advances the stage state machine - called unthrottled from controlTick()
// while a shot is running, since pulse timing can be sub-second
// (PREINFUSION_PULSE_MS_MIN). No-ops once in the final EXTRACTION stage (the
// common case for most of a shot's duration) or if no shot is running.
static void tickShotStages(unsigned long now) {
  if (!shotInProgress) return;
  if (currentStageIndex >= activeStageCount) return;
  ShotStage &stage = activeShotStages[currentStageIndex];
  if (stage.type == ShotStage::Type::EXTRACTION) return; // open-ended, nothing to advance to

  if (now - stageStartMillis < stage.durationMs) return;

  currentStageIndex++; // EXTRACTION is always the last stage, so this always stays in-bounds
  ShotStage &next = activeShotStages[currentStageIndex];
  applyShotStagePumpOutput(next);
  currentShotPhase = phaseForStageType(next.type);
  stageStartMillis = now;
}
```

Replace `startShot()` (`src/main.cpp:584-601`) with:

```cpp
void startShot() {
  if (shotInProgress) return;
  shotInProgress = true;
  shotStartMillis = millis();
  shotPeakTemp = currentTemperature;
  // Switch Brew to the more aggressive active-brewing gains immediately -
  // see config.h. Deliberately NOT a bumpless MANUAL/AUTOMATIC reset here:
  // we want the stronger Kp's instant response to apply right away, and the
  // gentle profile's small accumulated integral is harmless to inherit.
  refreshActiveProfileIfChanged();

  buildShotStages();
  currentStageIndex = 0;
  stageStartMillis = shotStartMillis;
  ShotStage &first = activeShotStages[0];
  currentShotPhase = phaseForStageType(first.type);
  applyShotStagePumpOutput(first);
}
```

In `stopShot()` (`src/main.cpp:603-635`), replace the single line
`setPumpRelay(true); // energize to interrupt - forces the pump off even if the switch is still held`
with:

```cpp
  pressureClosedLoopActive = false;
  pressurePID.SetMode(MANUAL);
  dimmerSetPowerPercent(0.0f); // force the pump off, same intent as the old relay's stopShot()
```

(The rest of `stopShot()` - shot-log append, Brew gain bumpless reset,
`shotCount` persistence - is unchanged.)

- [ ] **Step 5: Update `setup()`'s pin init**

Replace this block in `setup()` (`src/main.cpp:706-715`):

```cpp
  // Initialize/Configure Pins
  pinMode(PIN_SSR, OUTPUT);
  digitalWrite(PIN_SSR, LOW);
  // PIN_PUMP (HARDWARE_ROADMAP.md item 4, NC relay) boots de-energized -
  // unlike PIN_SSR's "always boot off" rule, de-energized here means the NC
  // contact is CLOSED (pass-through): the pump just follows the physical
  // Brew switch, with zero dependency on firmware having booted at all. This
  // is the deliberate fail-open default for this specific actuator - see
  // HARDWARE_ROADMAP.md item 4 for the reasoning.
  pinMode(PIN_PUMP, OUTPUT);
  digitalWrite(PIN_PUMP, PIN_PUMP_ACTIVE_LEVEL == HIGH ? LOW : HIGH);
```

with:

```cpp
  // Initialize/Configure Pins
  pinMode(PIN_SSR, OUTPUT);
  digitalWrite(PIN_SSR, LOW);

  // Pump pressure control (HARDWARE_ROADMAP.md items 7/8) - dimmer boots at
  // 0% (pump off) until a shot explicitly starts. Unlike the old relay's
  // fail-open default, a TRIAC dimmer has no passive pass-through state, so
  // "boots off" is the only safe default regardless of the physical Brew
  // switch position - an accepted tradeoff, see the design spec §3.
  dimmerInit();
  pressureSensorInit();
  pressurePID.SetOutputLimits(0, 100);
  pressurePID.SetMode(MANUAL);
```

- [ ] **Step 6: Extract the shared fault-window helper and add pressure fault detection to `controlTick()`**

Add this static helper immediately above `controlTick()` (`src/main.cpp:812`):

```cpp
// Rolling error-rate fault window (see config.h "Sensor fault detection"
// for why this replaced a consecutive-bad-read latch) - shared by both the
// temperature and pressure sensors. `window`/`index`/`filled`/`badCount`
// are the caller's own static state (one instance per sensor). Returns the
// updated fault status.
static bool updateFaultWindow(bool window[], int windowSize, int &index, int &filled,
                               int &badCount, int minSamples, float rateThreshold,
                               bool badRead) {
  if (window[index]) badCount--; // evict the slot being overwritten
  window[index] = badRead;
  if (badRead) badCount++;
  index = (index + 1) % windowSize;
  if (filled < windowSize) filled++;

  float badRate = (float)badCount / filled;
  return (filled >= minSamples) && (badRate >= rateThreshold);
}
```

In `controlTick()`, replace the temp sensor's inline fault-window block
(`src/main.cpp:821-839`):

```cpp
    static bool badWindow[SENSOR_FAULT_WINDOW] = {false};
    static int badWindowIndex = 0;
    static int badWindowFilled = 0;
    static int badCount = 0;

    bool badRead = (status != TempSensorStatus::OK);
    if (badWindow[badWindowIndex]) badCount--; // evict the slot being overwritten
    badWindow[badWindowIndex] = badRead;
    if (badRead) badCount++;
    badWindowIndex = (badWindowIndex + 1) % SENSOR_FAULT_WINDOW;
    if (badWindowFilled < SENSOR_FAULT_WINDOW) badWindowFilled++;

    float badRate = (float)badCount / badWindowFilled;
    bool newFault = (badWindowFilled >= SENSOR_FAULT_MIN_SAMPLES) &&
                    (badRate >= SENSOR_FAULT_RATE_THRESHOLD);
```

with:

```cpp
    static bool badWindow[SENSOR_FAULT_WINDOW] = {false};
    static int badWindowIndex = 0, badWindowFilled = 0, badCount = 0;

    bool badRead = (status != TempSensorStatus::OK);
    bool newFault = updateFaultWindow(badWindow, SENSOR_FAULT_WINDOW, badWindowIndex,
                                       badWindowFilled, badCount, SENSOR_FAULT_MIN_SAMPLES,
                                       SENSOR_FAULT_RATE_THRESHOLD, badRead);
```

(the `if (badRead) { Serial.println(...) }` / `if (newFault && !sensorFault) {...}`
logging and `sensorFault = newFault;` lines right after stay unchanged.)

The whole temp-read-and-fault block above is nested inside
`controlTick()`'s `if (now - lastTempReadTime >= TEMP_READ_INTERVAL) { ... }`
guard (throttled to the 250ms sensor read cadence). The new pressure block
goes **after that `if` block's closing `}`**, at `controlTick()`'s top
level - i.e. it runs every 50ms tick, not throttled to 250ms (pressure has
no equivalent slow UART round-trip forcing a slower cadence the way the
temp sensor does):

```cpp
  // Pressure read - joins the same tick cadence as everything else here (no
  // separate fast-poll interval needed; PID doesn't need faster than this,
  // and TRIAC firing itself is handled entirely by dimmer.cpp's zero-cross
  // ISR/hardware timer, outside this task altogether).
  {
    float bar;
    PressureSensorStatus pStatus = pressureSensorRead(bar);

    static bool pBadWindow[PRESSURE_FAULT_WINDOW] = {false};
    static int pBadIndex = 0, pBadFilled = 0, pBadCount = 0;
    bool pBadRead = (pStatus != PressureSensorStatus::OK);
    pressureFault = updateFaultWindow(pBadWindow, PRESSURE_FAULT_WINDOW, pBadIndex, pBadFilled,
                                       pBadCount, PRESSURE_FAULT_MIN_SAMPLES,
                                       PRESSURE_FAULT_RATE_THRESHOLD, pBadRead);
    if (!pressureFault && !pBadRead) currentPressure = bar;
  }
```

**Do not add the PID-compute/safety-ceiling logic here** - see Step 7a
below for why it belongs after `tickShotStages()` instead, and Step 8 for
the history-sampling change that goes right after this block.

- [ ] **Step 7: Drive the pressure PID + safety ceiling after the shot-stage tick**

The temperature PID's own `Compute()` call happens near the end of
`controlTick()`, well after `tickShotStages(now);` - the pressure PID
follows the same convention, so a stage transition that happens THIS tick
(e.g. entering a `PRESSURE_TARGET` stage) is already reflected in
`pressureClosedLoopActive`/`pressureSetpoint` before the PID computes
against it, instead of lagging one tick behind.

Find the existing `tickShotStages(now);` call in `controlTick()`
(`src/main.cpp:915`, inside the comment block starting "Shot stage state
machine - unthrottled..."). Add this immediately after that line:

```cpp
  // Pressure PID - only drives the dimmer while a pressure-targeting stage
  // is active (pressureClosedLoopActive, set by applyShotStagePumpOutput()
  // above, including any transition tickShotStages() just made this tick);
  // plain-duty stages already set the dimmer directly at their own stage
  // transition and don't need a per-tick update.
  if (pressureClosedLoopActive) {
    pressureInput = currentPressure;
    pressurePID.Compute();
    dimmerSetPowerPercent((float)pressureOutput);
  }

  // Safety ceiling always wins, independent of PID/profile output - same
  // rule as activeMaxSafety for temperature.
  if (pressureFault || currentPressure > PUMP_MAX_SAFETY_BAR) {
    dimmerSetPowerPercent(0.0f);
  }
```

- [ ] **Step 8: Sample pressure history alongside temp history**

In `controlTick()`'s history-sampling block (`src/main.cpp:892-899`),
change:

```cpp
  if (now - lastHistorySample >= TEMP_HISTORY_SAMPLE_INTERVAL_MS) {
    lastHistorySample = now;
    if (!sensorFault && currentTemperature > 0) {
      tempHistory[tempHistoryHead] = currentTemperature;
      tempHistoryHead = (tempHistoryHead + 1) % TEMP_HISTORY_LEN;
      if (tempHistoryCount < TEMP_HISTORY_LEN) tempHistoryCount++;
    }
  }
```

to:

```cpp
  if (now - lastHistorySample >= TEMP_HISTORY_SAMPLE_INTERVAL_MS) {
    lastHistorySample = now;
    if (!sensorFault && currentTemperature > 0) {
      tempHistory[tempHistoryHead] = currentTemperature;
      tempHistoryHead = (tempHistoryHead + 1) % TEMP_HISTORY_LEN;
      if (tempHistoryCount < TEMP_HISTORY_LEN) tempHistoryCount++;
    }
    if (!pressureFault) {
      pressureHistory[pressureHistoryHead] = currentPressure;
      pressureHistoryHead = (pressureHistoryHead + 1) % TEMP_HISTORY_LEN;
      if (pressureHistoryCount < TEMP_HISTORY_LEN) pressureHistoryCount++;
    }
  }
```

- [ ] **Step 9: Build**

Run: `& "C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\pio.exe" run -e esp32-s3-devkitc-1`
Expected: clean build. This is the biggest task in the plan - if it fails,
check every call site of the old `setPumpRelay`/`PIN_PUMP` was actually
replaced (search the whole `src/` tree for both names; there should be zero
remaining matches).

- [ ] **Step 10: Commit**

```bash
git add include/config.h src/main.cpp
git commit -m "Replace NC pump relay with dimmer + pressure PID in the shot-stage machine"
```

---

## Task 5: Profile schema — pressure fields

**Files:**
- Modify: `include/profiles.h`
- Modify: `src/profiles.cpp`
- Modify: `src/main.cpp` (`applyProfile()`, `setup()`'s NVS load block)

**Interfaces:**
- Consumes: `activePressureEnabled` and friends (Task 4's `main.cpp`
  globals).
- Produces: extended `profileGet()`/`profileSave()` signatures — consumed
  by Task 7 (`web.cpp`'s `/update` handler).

- [ ] **Step 1: Extend `include/profiles.h`**

Replace the `profileGet`/`profileSave` declarations with:

```cpp
// Loads profile `index`'s fields into the out-params. Returns false (params
// untouched) if index is out of range. Pressure fields default to
// disabled/0 for profiles saved before pressure control existed - no
// migration needed, ArduinoJson's `|` operator handles the missing keys.
bool profileGet(int index, String &name, double &temp, unsigned long &autoStopSec,
                bool &preinfusionEnabled, int &pulses, int &onMs, int &offMs,
                bool &pressureEnabled, double &pressureRampBar, unsigned long &pressureRampMs,
                bool &pressureDeclineEnabled, double &pressureDeclineBar,
                unsigned long &pressureDeclineMs);

// Adds a new profile (index == -1 or >= profileCount()) or overwrites an
// existing one. Returns the resulting index, or -1 if the list is already
// at PROFILE_MAX_COUNT and index requested a new entry. Commas in `name`
// are stripped (CSV storage, no quoting support - keeps this simple).
int profileSave(int index, String name, double temp, unsigned long autoStopSec,
                bool preinfusionEnabled, int pulses, int onMs, int offMs,
                bool pressureEnabled, double pressureRampBar, unsigned long pressureRampMs,
                bool pressureDeclineEnabled, double pressureDeclineBar,
                unsigned long pressureDeclineMs);
```

- [ ] **Step 2: Extend `src/profiles.cpp`**

In `addProfile()` (the internal seeding helper), add pressure defaults after
the existing `off_ms` line:

```cpp
static void addProfile(JsonArray &arr, const char *name, double temp,
                        unsigned long autoStopSec, bool preinfusionEnabled,
                        int pulses, int onMs, int offMs) {
  JsonObject p = arr.add<JsonObject>();
  p["name"] = name;
  p["temp"] = temp;
  p["auto_stop_sec"] = autoStopSec;
  p["preinfusion"] = preinfusionEnabled;
  p["pulses"] = pulses;
  p["on_ms"] = onMs;
  p["off_ms"] = offMs;
  // Pressure control (HARDWARE_ROADMAP.md item 8) - none of the seeded
  // starter profiles use it; disabled by default, same as every other
  // field's "no data yet" convention in this file.
  p["pressure_enabled"] = false;
  p["pressure_ramp_bar"] = 0.0;
  p["pressure_ramp_ms"] = 0;
  p["pressure_decline_enabled"] = false;
  p["pressure_decline_bar"] = 0.0;
  p["pressure_decline_ms"] = 0;
}
```

Replace `profileGet()` with:

```cpp
bool profileGet(int index, String &name, double &temp, unsigned long &autoStopSec,
                bool &preinfusionEnabled, int &pulses, int &onMs, int &offMs,
                bool &pressureEnabled, double &pressureRampBar, unsigned long &pressureRampMs,
                bool &pressureDeclineEnabled, double &pressureDeclineBar,
                unsigned long &pressureDeclineMs) {
  JsonDocument doc;
  if (!loadDoc(doc)) return false;
  JsonArray arr = doc.as<JsonArray>();
  if (index < 0 || index >= (int)arr.size()) return false;

  JsonObject p = arr[index];
  name = p["name"].as<String>();
  temp = p["temp"].as<double>();
  autoStopSec = p["auto_stop_sec"].as<unsigned long>();
  preinfusionEnabled = p["preinfusion"].as<bool>();
  pulses = p["pulses"].as<int>();
  onMs = p["on_ms"].as<int>();
  offMs = p["off_ms"].as<int>();
  // `| default` so profiles saved before this field existed (missing key)
  // deserialize as "pressure control disabled" instead of an unpredictable
  // JSON-null coercion.
  pressureEnabled = p["pressure_enabled"] | false;
  pressureRampBar = p["pressure_ramp_bar"] | 0.0;
  pressureRampMs = p["pressure_ramp_ms"] | 0UL;
  pressureDeclineEnabled = p["pressure_decline_enabled"] | false;
  pressureDeclineBar = p["pressure_decline_bar"] | 0.0;
  pressureDeclineMs = p["pressure_decline_ms"] | 0UL;
  return true;
}
```

Replace `profileSave()` with:

```cpp
int profileSave(int index, String name, double temp, unsigned long autoStopSec,
                bool preinfusionEnabled, int pulses, int onMs, int offMs,
                bool pressureEnabled, double pressureRampBar, unsigned long pressureRampMs,
                bool pressureDeclineEnabled, double pressureDeclineBar,
                unsigned long pressureDeclineMs) {
  if (name.length() > PROFILE_NAME_MAX_LEN) name = name.substring(0, PROFILE_NAME_MAX_LEN);
  if (name.length() == 0) name = "Profile";

  JsonDocument doc;
  loadDoc(doc); // ok if the file doesn't exist yet - doc just stays empty
  JsonArray arr = doc.is<JsonArray>() ? doc.as<JsonArray>() : doc.to<JsonArray>();
  int count = arr.size();

  JsonObject target;
  int targetIndex;
  if (index >= 0 && index < count) {
    targetIndex = index;
    target = arr[index];
  } else {
    if (count >= PROFILE_MAX_COUNT) return -1; // full
    targetIndex = count;
    target = arr.add<JsonObject>();
  }
  target["name"] = name;
  target["temp"] = temp;
  target["auto_stop_sec"] = autoStopSec;
  target["preinfusion"] = preinfusionEnabled;
  target["pulses"] = pulses;
  target["on_ms"] = onMs;
  target["off_ms"] = offMs;
  target["pressure_enabled"] = pressureEnabled;
  target["pressure_ramp_bar"] = pressureRampBar;
  target["pressure_ramp_ms"] = pressureRampMs;
  target["pressure_decline_enabled"] = pressureDeclineEnabled;
  target["pressure_decline_bar"] = pressureDeclineBar;
  target["pressure_decline_ms"] = pressureDeclineMs;

  saveDoc(doc);
  return targetIndex;
}
```

- [ ] **Step 3: Wire `main.cpp`'s `applyProfile()` to the new fields**

Replace `applyProfile()` (`src/main.cpp:466-494`) with:

```cpp
void applyProfile(int idx) {
  String name;
  double temp;
  unsigned long autoStop;
  bool preinfEnabled;
  int pulses, onMs, offMs;
  bool pressureEnabled, pressureDeclineEnabled;
  double pressureRampBar, pressureDeclineBar;
  unsigned long pressureRampMs, pressureDeclineMs;
  if (!profileGet(idx, name, temp, autoStop, preinfEnabled, pulses, onMs, offMs,
                   pressureEnabled, pressureRampBar, pressureRampMs,
                   pressureDeclineEnabled, pressureDeclineBar, pressureDeclineMs)) return;

  activeProfileIndex = idx;
  brewSetpoint = temp;
  shotAutoStopSec = autoStop;
  activePreinfusionEnabled = preinfEnabled;
  activePreinfusionPulses = pulses;
  activePreinfusionOnMs = onMs;
  activePreinfusionOffMs = offMs;
  activePressureEnabled = pressureEnabled;
  activePressureRampBar = pressureRampBar;
  activePressureRampMs = pressureRampMs;
  activePressureDeclineEnabled = pressureDeclineEnabled;
  activePressureDeclineBar = pressureDeclineBar;
  activePressureDeclineMs = pressureDeclineMs;

  Preferences preferences;
  preferences.begin("gaggia", false);
  preferences.putInt("active_profile", activeProfileIndex);
  preferences.putDouble("brew_target", brewSetpoint);
  preferences.putULong("shot_auto_stop", shotAutoStopSec);
  preferences.putBool("pi_enabled", activePreinfusionEnabled);
  preferences.putInt("pi_pulses", activePreinfusionPulses);
  preferences.putInt("pi_on_ms", activePreinfusionOnMs);
  preferences.putInt("pi_off_ms", activePreinfusionOffMs);
  preferences.putBool("press_en", activePressureEnabled);
  preferences.putDouble("press_ramp_bar", activePressureRampBar);
  preferences.putULong("press_ramp_ms", activePressureRampMs);
  preferences.putBool("press_dec_en", activePressureDeclineEnabled);
  preferences.putDouble("press_dec_bar", activePressureDeclineBar);
  preferences.putULong("press_dec_ms", activePressureDeclineMs);
  preferences.end();

  refreshActiveProfileIfChanged();
}
```

- [ ] **Step 4: Load the new NVS keys at boot**

In `setup()`'s NVS load block (`src/main.cpp`, right after the existing
`activePreinfusionOffMs = preferences.getInt("pi_off_ms", PREINFUSION_OFF_MS_DEFAULT);`
line), add:

```cpp
  activePressureEnabled = preferences.getBool("press_en", false);
  activePressureRampBar = preferences.getDouble("press_ramp_bar", PRESSURE_RAMP_BAR_DEFAULT);
  activePressureRampMs = preferences.getULong("press_ramp_ms", PRESSURE_RAMP_MS_DEFAULT);
  activePressureDeclineEnabled = preferences.getBool("press_dec_en", false);
  activePressureDeclineBar = preferences.getDouble("press_dec_bar", PRESSURE_DECLINE_BAR_DEFAULT);
  activePressureDeclineMs = preferences.getULong("press_dec_ms", PRESSURE_DECLINE_MS_DEFAULT);
```

Also load persisted pressure PID tunings, right after the existing
`steamMaxSafety = preferences.getDouble("steam_max_safety", STEAM_MAX_SAFETY_DEFAULT);`
line:

```cpp
  pressureKp = preferences.getDouble("press_kp", PUMP_PRESSURE_KP_DEFAULT);
  pressureKi = preferences.getDouble("press_ki", PUMP_PRESSURE_KI_DEFAULT);
  pressureKd = preferences.getDouble("press_kd", PUMP_PRESSURE_KD_DEFAULT);
```

**Ordering note:** the `Preferences` load block runs *before* Task 4 Step
5's pin-init block only in the sense that both are in `setup()` - actually
check the real order: pin-init (where Task 4 Step 5 put
`pressurePID.SetMode(MANUAL);`) runs **before** the `Preferences` block that
loads `pressureKp/Ki/Kd`. So `SetTunings()` must NOT go right after
`SetMode(MANUAL)` (it would apply stale defaults, not the loaded values).
Instead, add it in the existing "Initialize PID" section further down in
`setup()`, right after `myPID.SetOutputLimits(0, WindowSize);` (which
already runs after the `Preferences` block):

```cpp
  windowStartTime = millis();
  myPID.SetOutputLimits(0, WindowSize);
  pressurePID.SetTunings(pressureKp, pressureKi, pressureKd);
  setOpMode(OpMode::OFF);
  noteActivity();
```

(This replaces the original three-line
`windowStartTime = millis(); myPID.SetOutputLimits(0, WindowSize); setOpMode(OpMode::OFF); noteActivity();`
block with the same lines plus the new `pressurePID.SetTunings(...)` call
inserted between them.)

- [ ] **Step 5: Build**

Run: `& "C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\pio.exe" run -e esp32-s3-devkitc-1`
Expected: clean build.

- [ ] **Step 6: Commit**

```bash
git add include/profiles.h src/profiles.cpp src/main.cpp
git commit -m "Extend profile schema with pressure-stage fields"
```

---

## Task 6: web.cpp — /status pressure telemetry

**Files:**
- Modify: `src/web.cpp` (`handleStatus()`)

**Interfaces:**
- Consumes: `currentPressure`, `pressureFault`, `pressureHistory[]`,
  `pressureHistoryHead`, `pressureHistoryCount`, `activePressureEnabled`
  and friends, `pressureKp/Ki/Kd` (Task 4/5's `main.cpp` globals).
- Produces: `/status` JSON fields `pressure`, `pressure_fault`,
  `pressure_history`, `press_enabled`, `press_ramp_bar`, `press_ramp_ms`,
  `press_decline_enabled`, `press_decline_bar`, `press_decline_ms`,
  `press_kp`, `press_ki`, `press_kd` — consumed by Task 8's JS.

- [ ] **Step 1: Snapshot the new fields under the lock**

In `handleStatus()`, right after the existing
`int snapHistoryCount = tempHistoryCount, snapHistoryHead = tempHistoryHead;`
block (`src/web.cpp:1577-1582`), add:

```cpp
  bool snapPressureFault = pressureFault;
  float snapPressure = currentPressure;
  bool snapPressEnabled = activePressureEnabled;
  double snapPressRampBar = activePressureRampBar;
  unsigned long snapPressRampMs = activePressureRampMs;
  bool snapPressDeclineEnabled = activePressureDeclineEnabled;
  double snapPressDeclineBar = activePressureDeclineBar;
  unsigned long snapPressDeclineMs = activePressureDeclineMs;
  double snapPressKp = pressureKp, snapPressKi = pressureKi, snapPressKd = pressureKd;
  int snapPressHistoryCount = pressureHistoryCount, snapPressHistoryHead = pressureHistoryHead;
  float snapPressHistory[TEMP_HISTORY_LEN];
  for (int i = 0; i < snapPressHistoryCount; i++) {
    int idx = (snapPressHistoryHead - snapPressHistoryCount + i + TEMP_HISTORY_LEN * 2) % TEMP_HISTORY_LEN;
    snapPressHistory[i] = pressureHistory[idx];
  }
```

- [ ] **Step 2: Add the JSON fields**

Right after the existing `json += ",\"pi_off_ms\":" + String(snapPiOffMs);`
line (`src/web.cpp:1676`), add:

```cpp
  json += ",\"pressure\":" + String(snapPressure, 2);
  json += ",\"pressure_fault\":" + String(snapPressureFault ? "true" : "false");
  json += ",\"press_enabled\":" + String(snapPressEnabled ? "true" : "false");
  json += ",\"press_ramp_bar\":" + String(snapPressRampBar);
  json += ",\"press_ramp_ms\":" + String(snapPressRampMs);
  json += ",\"press_decline_enabled\":" + String(snapPressDeclineEnabled ? "true" : "false");
  json += ",\"press_decline_bar\":" + String(snapPressDeclineBar);
  json += ",\"press_decline_ms\":" + String(snapPressDeclineMs);
  json += ",\"press_kp\":" + String(snapPressKp, 4);
  json += ",\"press_ki\":" + String(snapPressKi, 4);
  json += ",\"press_kd\":" + String(snapPressKd, 4);
```

- [ ] **Step 3: Add `PRESSURE` to the `shot_phase` switch**

Change the switch at `src/web.cpp:1678-1683` from:

```cpp
  switch (snapShotPhase) {
    case ShotPhase::PREINFUSION_ON:
    case ShotPhase::PREINFUSION_OFF: json += "preinfusion"; break;
    case ShotPhase::EXTRACTION: json += "extraction"; break;
    default: json += "none"; break;
  }
```

to:

```cpp
  switch (snapShotPhase) {
    case ShotPhase::PREINFUSION_ON:
    case ShotPhase::PREINFUSION_OFF: json += "preinfusion"; break;
    case ShotPhase::PRESSURE: json += "pressure"; break;
    case ShotPhase::EXTRACTION: json += "extraction"; break;
    default: json += "none"; break;
  }
```

- [ ] **Step 4: Add the pressure history array**

Right after the existing temp `json += "]";` that closes the `"history"`
array (`src/web.cpp:1701`, just before the final `json += "}";`), add:

```cpp
  json += ",\"pressure_history\":[";
  for (int i = 0; i < snapPressHistoryCount; i++) {
    if (i > 0) json += ",";
    json += String(snapPressHistory[i], 2);
  }
  json += "]";
```

- [ ] **Step 5: Build**

Run: `& "C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\pio.exe" run -e esp32-s3-devkitc-1`
Expected: clean build.

- [ ] **Step 6: Commit**

```bash
git add src/web.cpp
git commit -m "Add pressure telemetry to /status"
```

---

## Task 7: web.cpp — /update handler for pressure profile fields + PID tuning

**Files:**
- Modify: `src/web.cpp` (`handleUpdate()`)

**Interfaces:**
- Consumes: extended `profileSave()` (Task 5); `pressureKp/Ki/Kd` and
  `pressurePID` (Task 4).
- Produces: `/update` query params `profile_press_enabled`,
  `profile_press_ramp_bar`, `profile_press_ramp_ms`,
  `profile_press_decline_enabled`, `profile_press_decline_bar`,
  `profile_press_decline_ms`, `press_kp`, `press_ki`, `press_kd` — consumed
  by Task 9's JS.

- [ ] **Step 1: Extend the `profile_save` block**

Replace the `if (hasArg("profile_save")) { ... }` block
(`src/web.cpp:1891-1910`) with:

```cpp
  if (hasArg("profile_save")) {
    int idx = hasArg("profile_index") ? arg("profile_index").toInt() : -1;
    String name = hasArg("profile_name") ? arg("profile_name") : "Profile";
    double temp = hasArg("profile_temp") ? arg("profile_temp").toDouble() : BREW_SETPOINT_DEFAULT;
    unsigned long autoStop = hasArg("profile_autostop")
        ? constrain((long)arg("profile_autostop").toInt(), (long)SHOT_AUTO_STOP_SEC_MIN, (long)SHOT_AUTO_STOP_SEC_MAX)
        : SHOT_AUTO_STOP_SEC_DEFAULT;
    bool piEnabled = hasArg("profile_pi_enabled") && arg("profile_pi_enabled") == "1";
    int pulses = hasArg("profile_pi_pulses")
        ? constrain(arg("profile_pi_pulses").toInt(), 0, PREINFUSION_PULSES_MAX) : 0;
    int onMs = hasArg("profile_pi_on_ms")
        ? constrain(arg("profile_pi_on_ms").toInt(), PREINFUSION_PULSE_MS_MIN, PREINFUSION_PULSE_MS_MAX) : PREINFUSION_ON_MS_DEFAULT;
    int offMs = hasArg("profile_pi_off_ms")
        ? constrain(arg("profile_pi_off_ms").toInt(), PREINFUSION_PULSE_MS_MIN, PREINFUSION_PULSE_MS_MAX) : PREINFUSION_OFF_MS_DEFAULT;
    bool pressureEnabled = hasArg("profile_press_enabled") && arg("profile_press_enabled") == "1";
    double pressureRampBar = hasArg("profile_press_ramp_bar")
        ? arg("profile_press_ramp_bar").toDouble() : PRESSURE_RAMP_BAR_DEFAULT;
    unsigned long pressureRampMs = hasArg("profile_press_ramp_ms")
        ? (unsigned long)arg("profile_press_ramp_ms").toInt() : PRESSURE_RAMP_MS_DEFAULT;
    bool pressureDeclineEnabled = hasArg("profile_press_decline_enabled") && arg("profile_press_decline_enabled") == "1";
    double pressureDeclineBar = hasArg("profile_press_decline_bar")
        ? arg("profile_press_decline_bar").toDouble() : PRESSURE_DECLINE_BAR_DEFAULT;
    unsigned long pressureDeclineMs = hasArg("profile_press_decline_ms")
        ? (unsigned long)arg("profile_press_decline_ms").toInt() : PRESSURE_DECLINE_MS_DEFAULT;
    int saved = profileSave(idx, name, temp, autoStop, piEnabled, pulses, onMs, offMs,
                             pressureEnabled, pressureRampBar, pressureRampMs,
                             pressureDeclineEnabled, pressureDeclineBar, pressureDeclineMs);
    // Editing the profile that's currently active also refreshes the live
    // settings from it, so tweaking "your current setup" takes effect
    // immediately instead of silently drifting from what's now saved.
    if (saved >= 0 && saved == activeProfileIndex) applyProfile(saved);
  }
```

- [ ] **Step 2: Add pump-pressure Kp/Ki/Kd tuning**

Right after the existing Steam Kp/Ki/Kd block ends (after
`src/web.cpp:1763`'s `preferences.putDouble("steam_kd", steamKd);` and its
closing brace), add a new block:

```cpp
  if (hasArg("press_kp")) {
    pressureKp = arg("press_kp").toDouble();
    preferences.putDouble("press_kp", pressureKp);
  }
  if (hasArg("press_ki")) {
    pressureKi = arg("press_ki").toDouble();
    preferences.putDouble("press_ki", pressureKi);
  }
  if (hasArg("press_kd")) {
    pressureKd = arg("press_kd").toDouble();
    preferences.putDouble("press_kd", pressureKd);
  }
  if (hasArg("press_kp") || hasArg("press_ki") || hasArg("press_kd")) {
    pressurePID.SetTunings(pressureKp, pressureKi, pressureKd);
  }
```

- [ ] **Step 3: Build**

Run: `& "C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\pio.exe" run -e esp32-s3-devkitc-1`
Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add src/web.cpp
git commit -m "Add pressure profile fields and PID tuning to /update"
```

---

## Task 8: web.cpp — dashboard pressure chart + pump-pressure tuning card

**Files:**
- Modify: `src/web.cpp` (HTML + JS)

**Interfaces:**
- Consumes: `/status` fields from Task 6 (`pressure`, `pressure_fault`,
  `pressure_history`, `press_kp/ki/kd`).

- [ ] **Step 1: Add the pressure chart card**

Right after the existing temp chart-card closes (`src/web.cpp:446-449`),
add a second `.chart-card` (the CSS comment at `src/web.cpp:253-255`
already anticipated this exact addition):

```html
        <div class="chart-card">
          <div class="chart-label"><span>Pressure &middot; last 2 min</span><span id="pressure_label">-- bar</span></div>
          <canvas id="pressure_chart" width="300" height="60"></canvas>
        </div>
```

- [ ] **Step 2: Add a pump-pressure tuning card**

After the existing Steam tuning card's closing `</div>` (`src/web.cpp:504`,
right after the Steam form's `</form>`), add a new card using the exact
same plain `<form action="/update" method="GET">` pattern the Brew/Steam
cards use (a full-page GET submit, not a JS/XHR call), with
`press_kp`/`press_ki`/`press_kd` as the field names/ids:

```html
      <div class="card">
        <div class="tab-section-title">Pump Pressure</div>
        <form action="/update" method="GET">
          <p class="hint">Closed-loop control for the pressure ramp/decline stages of a shot profile (HARDWARE_ROADMAP.md item 8). Has no effect on plain on/off pre-infusion pulses.</p>
          <div class="field-row-3">
            <div class="field"><label for="input_press_kp">Kp</label><input type="number" step="any" name="press_kp" id="input_press_kp" value=""></div>
            <div class="field"><label for="input_press_ki">Ki</label><input type="number" step="0.01" name="press_ki" id="input_press_ki" value=""></div>
            <div class="field"><label for="input_press_kd">Kd</label><input type="number" step="any" name="press_kd" id="input_press_kd" value=""></div>
          </div>
          <button type="submit" class="submit">Save Pump Pressure</button>
        </form>
      </div>
```

- [ ] **Step 3: Add the JS wiring**

Add a `drawPressureSparkline()` function right after the existing
`drawSparkline()` function (`src/web.cpp:765-801`):

```js
function drawPressureSparkline(data) {
  var canvas = document.getElementById("pressure_chart");
  if (!canvas || !data || data.length < 2) return;
  var ctx = canvas.getContext("2d");
  var w = canvas.width, h = canvas.height;
  ctx.clearRect(0, 0, w, h);
  var min = Math.min.apply(null, data), max = Math.max.apply(null, data);
  if (max - min < 0.5) { max += 0.25; min -= 0.25; }
  if (min > 0) min = 0; // pressure chart always includes zero for scale

  ctx.beginPath();
  data.forEach(function (v, i) {
    var x = (i / (data.length - 1)) * w;
    var y = h - ((v - min) / (max - min)) * (h - 6) - 3;
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  });
  ctx.strokeStyle = "#3f8cd9";
  ctx.lineWidth = 2.5;
  ctx.lineJoin = "round";
  ctx.stroke();
}
```

In the status-poll handler (find the call to `drawSparkline(json.history);`
at `src/web.cpp:1078`), add right after it:

```js
      drawPressureSparkline(json.pressure_history);
      var pLabel = document.getElementById("pressure_label");
      if (pLabel) pLabel.textContent = (json.pressure_fault ? "fault" : json.pressure.toFixed(2) + " bar");
```

Find the `setVal("input_steam_kp", json.steam_kp);` line
(`src/web.cpp:1117`) and its neighboring `setVal("input_steam_...")` calls;
add alongside them:

```js
      setVal("input_press_kp", json.press_kp);
      setVal("input_press_ki", json.press_ki);
      setVal("input_press_kd", json.press_kd);
```

- [ ] **Step 4: Build**

Run: `& "C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\pio.exe" run -e esp32-s3-devkitc-1`
Expected: clean build (this task is HTML/JS embedded in a C++ string
literal - a clean build is the only automated check available; the actual
page rendering is verified in Task 10).

- [ ] **Step 5: Commit**

```bash
git add src/web.cpp
git commit -m "Add pressure chart and pump-pressure tuning card to the dashboard"
```

---

## Task 9: web.cpp — profile editor pressure fields

**Files:**
- Modify: `src/web.cpp` (profile editor HTML + JS)

**Interfaces:**
- Consumes: Task 7's `/update?profile_save=...` pressure params; Task 6's
  `profilesReadJson()` output (already carries the new fields via Task 5).

- [ ] **Step 1: Fix the stale pre-infusion hint text**

Change (`src/web.cpp:526`):

```html
          <p class="hint" style="margin-top:var(--sp-2)">Cycles the pump on/off a few times before switching to continuous power - approximates the puck-saturation benefit of true low-pressure pre-infusion using just an on/off relay. Needs the pump relay wired (HARDWARE_ROADMAP.md item 4) to have any physical effect - software-only until then.</p>
```

to:

```html
          <p class="hint" style="margin-top:var(--sp-2)">Cycles the pump on/off a few times before switching to continuous power - approximates the puck-saturation benefit of true low-pressure pre-infusion. Needs the dimmer wired (HARDWARE_ROADMAP.md item 8) to have any physical effect.</p>
```

- [ ] **Step 2: Add pressure fields to the profile editor form**

Right after the existing pre-infusion `field-row-3` block
(`src/web.cpp:527-531`) and before the `chart-card` preview block, add:

```html
          <label class="check-row"><input type="checkbox" id="input_profile_press_enabled"> Pressure profile</label>
          <p class="hint" style="margin-top:var(--sp-2)">Closed-loop pressure ramp, held for a duration, then an optional decline near the end of the shot. Needs the transducer + dimmer wired (HARDWARE_ROADMAP.md items 7/8).</p>
          <div class="field-row-3" style="margin-top:var(--sp-3)">
            <div class="field"><label for="input_profile_press_ramp_bar">Ramp target (bar)</label><input type="number" step="0.1" id="input_profile_press_ramp_bar" value="9"></div>
            <div class="field"><label for="input_profile_press_ramp_sec">Ramp/hold (sec)</label><input type="number" step="1" min="1" id="input_profile_press_ramp_sec" value="20"></div>
          </div>
          <label class="check-row"><input type="checkbox" id="input_profile_press_decline_enabled"> Declining finish</label>
          <div class="field-row-3" style="margin-top:var(--sp-3)">
            <div class="field"><label for="input_profile_press_decline_bar">Decline target (bar)</label><input type="number" step="0.1" id="input_profile_press_decline_bar" value="6"></div>
            <div class="field"><label for="input_profile_press_decline_sec">Decline (sec)</label><input type="number" step="1" min="1" id="input_profile_press_decline_sec" value="8"></div>
          </div>
```

- [ ] **Step 3: Wire `editProfile()`, `newProfileForm()`, `submitProfileForm()`**

In `editProfile()` (`src/web.cpp:1388-1403`), add right after the existing
`document.getElementById("input_profile_pi_off").value = p.off_ms / 1000;`
line:

```js
  document.getElementById("input_profile_press_enabled").checked = p.pressure_enabled;
  document.getElementById("input_profile_press_ramp_bar").value = p.pressure_ramp_bar;
  document.getElementById("input_profile_press_ramp_sec").value = p.pressure_ramp_ms / 1000;
  document.getElementById("input_profile_press_decline_enabled").checked = p.pressure_decline_enabled;
  document.getElementById("input_profile_press_decline_bar").value = p.pressure_decline_bar;
  document.getElementById("input_profile_press_decline_sec").value = p.pressure_decline_ms / 1000;
```

In `newProfileForm()` (`src/web.cpp:1405-1417`), add right after the
existing `document.getElementById("input_profile_pi_off").value = 2;` line:

```js
  document.getElementById("input_profile_press_enabled").checked = false;
  document.getElementById("input_profile_press_ramp_bar").value = 9;
  document.getElementById("input_profile_press_ramp_sec").value = 20;
  document.getElementById("input_profile_press_decline_enabled").checked = false;
  document.getElementById("input_profile_press_decline_bar").value = 6;
  document.getElementById("input_profile_press_decline_sec").value = 8;
```

In `submitProfileForm()` (`src/web.cpp:1447-1457`), add right after the
existing `q += "&profile_pi_off_ms=" + ...` line:

```js
  q += "&profile_press_enabled=" + (document.getElementById("input_profile_press_enabled").checked ? "1" : "0");
  q += "&profile_press_ramp_bar=" + document.getElementById("input_profile_press_ramp_bar").value;
  q += "&profile_press_ramp_ms=" + Math.round(document.getElementById("input_profile_press_ramp_sec").value * 1000);
  q += "&profile_press_decline_enabled=" + (document.getElementById("input_profile_press_decline_enabled").checked ? "1" : "0");
  q += "&profile_press_decline_bar=" + document.getElementById("input_profile_press_decline_bar").value;
  q += "&profile_press_decline_ms=" + Math.round(document.getElementById("input_profile_press_decline_sec").value * 1000);
```

- [ ] **Step 4: Build**

Run: `& "C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\pio.exe" run -e esp32-s3-devkitc-1`
Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add src/web.cpp
git commit -m "Add pressure-profile fields to the shot profile editor"
```

---

## Task 10: Bench Milestone A — dimmer bring-up, relay decommission, on/off pass-through

Hardware/procedure task, no further code changes expected unless bench
testing surfaces a bug.

**Prerequisites:** Tasks 1-9 flashed to the board over USB (bench, mains
disconnected, per `AGENTS.md` §6).

- [ ] **Step 1: Bench-test the dimmer on a lamp load**

Wire the dimmer module's AC side to a mains lamp (not the pump), its
control side to `PIN_DIMMER_GATE`/`PIN_DIMMER_ZC`/3V3/GND. Power the ESP32
over USB only. Drive `dimmerSetPowerPercent(50)` via a quick temporary
Serial command or by starting a shot with a profile that has no pre-infusion
and no pressure control (falls straight to `EXTRACTION` at plain 100% -
temporarily hardcode a lower test percent if needed, revert after). Confirm
the lamp dims smoothly and zero-cross timing looks stable (no flicker at a
fixed percent) **first at idle, then specifically under WiFi load** (open
the Web UI and leave it polling `/status` every 2s, or start an OTA-sized
transfer) - the gate-fire callback runs in the esp_timer task rather than a
true hardware ISR on this build (confirmed during final review: this
board's sdkconfig doesn't enable `ESP_TIMER_ISR` dispatch), so idle-only
testing would not actually exercise the jitter this architecture is meant
to survive. Visible flicker appearing only under WiFi load is the tell that
this needs revisiting with real hardware to validate a fix against,
rather than guessing at one now.

- [ ] **Step 2: Physically disconnect the old relay's control-side wiring**

Unwire GPIO5/VCC/GND from the SONGLE SRD-05VDC-SL-C relay module (item 4).
It was never mains-spliced, so there is nothing else to undo.

- [ ] **Step 3: Splice the dimmer at the pump's White (switched) wire**

Same splice point already identified for item 4/8 in
`HARDWARE_ROADMAP.md` - the pump's White wire (switched, from the Brew
Switch), confirmed via the OEM service diagram and the user's own
hand-tracing. Insulate every joint before proceeding. Machine unplugged
from mains throughout.

- [ ] **Step 4: First live power-up - Milestone A**

Plug into mains. Start a shot with a profile that has no pre-infusion and
no pressure control enabled (plain 100% duty through `EXTRACTION`) - confirm
the pump runs exactly as it did before this change. Stop the shot - confirm
the pump stops. This alone delivers everything the old relay would have.

- [ ] **Step 5: Commit any bugfixes found during this milestone**

If bench testing surfaces a real firmware bug, fix it, rebuild, reflash,
retest, then:

```bash
git add -A
git commit -m "Fix issue found during dimmer/relay-decommission bring-up"
```

(Skip this step entirely if nothing needed fixing.)

---

## Task 11: Bench Milestone B — transducer plumbing + calibration

**Prerequisites:** Task 10 complete (dimmer verified on the real pump).

- [ ] **Step 1: Plumb the transducer**

Install the T-fitting at the pump outlet, mount the transducer, wire its
analog output through the divider (if needed) to `PIN_PRESSURE_ADC`.
Machine unplugged from mains during any wiring.

- [ ] **Step 2: Calibrate `PRESSURE_SENSOR_ZERO_MV`/`PRESSURE_SENSOR_MV_PER_BAR`**

With the pump off and the circuit at atmospheric pressure (0 bar), read the
raw millivolt value (temporarily log it via `Serial.printf` in
`pressureSensorRead()`, or read `/status`'s `pressure` field with the
placeholder calibration and back-calculate). Update
`PRESSURE_SENSOR_ZERO_MV` in `include/config.h` to match. If a second
reference point is available (e.g. the transducer's own datasheet span
voltage at its rated max), use it to correct `PRESSURE_SENSOR_MV_PER_BAR`
too; otherwise keep the datasheet-derived default and refine once real shots
give a plausible working-pressure range to sanity-check against.

- [ ] **Step 3: Verify live readings - Milestone B**

Pull a shot (plain duty is fine) and watch `/status`'s `pressure` field or
the dashboard chart - confirm it rises when the pump runs and falls when it
stops, roughly tracking what's physically happening (a rough correlation is
enough here; this is not yet closed-loop control).

- [ ] **Step 4: Commit the calibration constants**

```bash
git add include/config.h
git commit -m "Calibrate pressure transducer zero/span from bench measurement"
```

---

## Task 12: Bench Milestone C — closed-loop pressure tuning

**Prerequisites:** Task 11 complete (transducer verified reading real
pressure).

- [ ] **Step 1: Enable a pressure profile**

In the Web UI profile editor, enable "Pressure profile" on a test profile
with a modest ramp target (e.g. 6 bar) and a generous ramp/hold duration, no
decline yet. Save it, load it as the active profile.

- [ ] **Step 2: Pull a shot and observe**

Start a shot. Watch the pressure chart and the `shot_phase` field (should
read `pressure` during the ramp/hold stage). Note overshoot/oscillation/
settling behavior, the same way brew-temperature PID tuning was iterated
against real data (`AGENTS.md` §10 change log).

- [ ] **Step 3: Iterate `press_kp`/`press_ki`/`press_kd` via the Web UI**

Adjust the Pump Pressure Tuning card's Kp/Ki/Kd live, re-test, repeat until
the ramp reaches target with acceptable overshoot/settling. This is
expected to take real iteration, not a single pass.

- [ ] **Step 4: Raise the target toward ~9 bar and re-tune if needed**

Once 6 bar is stable, raise the ramp target profile-by-profile toward the
working ~9 bar target, re-checking behavior at each step rather than jumping
straight there.

- [ ] **Step 5: Enable and tune the decline stage**

Turn on "Declining finish" on the test profile with a lower target and a
short duration near the end of a typical shot length; verify the stage
transition and pressure drop behave as expected.

- [ ] **Step 6: No commit needed**

This task only changes NVS-persisted tuning values (already saved live via
the Web UI, not source-controlled) and profile JSON on the device's own
LittleFS. If any firmware bug was found and fixed along the way, commit that
fix separately as in Task 10 Step 5.

---

## Task 13: Documentation — AGENTS.md + HARDWARE_ROADMAP.md

**Files:**
- Modify: `AGENTS.md` (Section 8 roadmap item 4/7/8 entries, Section 10
  change log)
- Modify: `HARDWARE_ROADMAP.md` (items 4, 7, 8 status)

**Interfaces:** None (documentation only).

- [ ] **Step 1: Update `AGENTS.md` Section 8**

Mark roadmap items 7 and 8 as shipped (or "on the bench, tuning in
progress" if Task 12 is still ongoing at the time this is written), and
update item 4's entry to note the relay was fully removed (not just
"folded into item 8") once this plan's Task 4/10 actually happened, since
the existing entries only describe the *plan* to do so.

- [ ] **Step 2: Add a Section 10 change-log entry**

Following the existing format (newest entry at the top of the Change Log
section), summarize: relay removed, dimmer + transducer bring-up, the
`ShotStage`/`PRESSURE_TARGET` extension, the profile schema addition, the
bench milestones actually reached, and the calibration constants recorded
in Task 11. Reference this plan's spec
(`docs/superpowers/specs/2026-09-04-pump-pressure-bringup-design.md`) and
this plan file for full detail rather than duplicating it.

- [ ] **Step 3: Update `HARDWARE_ROADMAP.md`**

Update items 4 (relay - mark fully removed, point to item 8), 7
(transducer - mark done, note the calibration constants), and 8 (dimmer -
mark the on/off half done; note the closed-loop half's status depending on
how far Task 12 got).

- [ ] **Step 4: Commit**

```bash
git add AGENTS.md HARDWARE_ROADMAP.md
git commit -m "Document pump-pressure hardware bring-up in AGENTS.md/HARDWARE_ROADMAP.md"
```
