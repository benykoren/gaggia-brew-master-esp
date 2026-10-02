# Phase Profiles + Open-Loop Pump Model + Dimmer Hardening — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the fixed pulse/ramp/decline shot system with phase-list profiles driven by a calibrated open-loop pump model, harden the pulse-skip dimmer, and add a 3-run scale calibration — all ready to switch to closed-loop pressure control when a 0–16 bar sensor is fitted.

**Architecture:** The new logic lives in four pure C++ units (`pump_model`, `pump_cal_math`, `profile_codec`, `shot_planner`) with no Arduino headers, so they unit-test on the PC (`env:native`). Device-side units wrap them:
- `dimmer.cpp` (hardened);
- `pump_calibration.cpp` (run state machine + NVS);
- `profiles.cpp` (LittleFS store + migration);
- `main.cpp` (the planner runs in `controlTick()`);
- `web.cpp` (API + UI).

**Tech Stack:** PlatformIO, Arduino-ESP32 3.x (pioarduino) on ESP32-S3, ESP-IDF `gptimer`/`gpio` drivers, ArduinoJson 7, ESPAsyncWebServer, Unity (native tests).

**Spec:** [`docs/superpowers/specs/2026-10-02-phase-profiles-open-loop-pump-design.md`](../specs/2026-10-02-phase-profiles-open-loop-pump-design.md) — read it first; this plan argues from it.

## Global Constraints

- Work on branch `feature/phase-profiles-open-loop-pump` (already created from `origin/main` @ `081e068`). Never commit to `main`.
- **Commit messages** end with the trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- **PlatformIO:** `$pio = "C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\pio.exe"` (PowerShell). Device build: `& $pio run -e esp32-s3-devkitc-1`. Native tests: `& $pio test -e native`.
- **Pure units** (`pump_model`, `pump_cal_math`, `profile_codec`, `shot_planner`, `profile_types.h`) must not include `Arduino.h`, `Preferences.h`, `LittleFS.h` or use Arduino `String`. They may include `config.h` (macros and enums only) and `ArduinoJson.h`.
- **ISR code** in `dimmer.cpp` must stay integer-only. No float in anything reachable from `onZeroCross()` or `onGateAlarm()` (existing FPU-safety note, 2026-09-15 crash).
- **Safety rules don't change.** The forced-0% pump cutoffs (pressure fault while the closed loop is active; `currentPressure > PUMP_MAX_SAFETY_BAR`; shot stop) still run after the planner in the same tick and win. Heater logic is untouched.
- `PRESSURE_CLOSED_LOOP_AVAILABLE` stays `false`. The closed-loop path is compiled but inactive.
- **Profile limits** (from the spec, plus the planning adjustments listed below):
  - user profiles ≤ 8 (`PROFILE_MAX_COUNT`);
  - phases 1–8 (utility profiles ≤ 10);
  - name ≤ 20 chars, phase name ≤ 15;
  - `pressure_scale` 0.7–1.1;
  - `trim_pct` −20…+20;
  - `max_sec` 5–90 (utility profiles ≤ 120);
  - pressure targets 0–11 bar, flow 0–10 ml/s, power 0–100%;
  - `duration_s` 0.2–120, `ramp_s` 0 up to `duration_s`.
- **Calibration:** 3 runs of `PUMP_CAL_RUN_MS = 10000` at 100% with no slew. Each run must be 0.15–0.40 ml/stroke, and no run may be more than 10% from the mean. Default `PUMP_ML_PER_CLICK_DEFAULT = 0.213`.
- **Pump model constants:** compliance C = 1.4 ml/bar, τ = 0.5 s, reference puck 2.0 ml/s at 9 bar, headspace 30 ml, shape floor 0.02.
- **Dimmer:** gate hold 1500 µs from the zero-cross edge; slew +250 %/s (decreases are instant); mains ABSENT after 100 ms without a crossing; valid range 45–65 Hz.
- **User-supplied names** reach the DOM only through `textContent` / `createElement`, never through `innerHTML`. In `/status` JSON they go through `jsonEscape()`.
- **Flashing:** over USB (OTA has been unreliable). Hold BOOT and tap RESET if the upload fails.

### Planning adjustments to the spec (found while reading the code; called out so reviewers don't flag them as drift)

1. **Default ml/click is 0.213, not 0.2225.** 0.2225 is Gaggiuino's internal `pumpFlowAtZero` before its −0.045 offset and ×1.2 multiplier. Their *effective* ml/click at 0 bar is 0.213.
2. **Added a 30 ml headspace to the model.** `P_est` stays 0 until 30 ml has been pumped. Without this, flow-fill phases hit their bar limit in about 1 s and starve the fill.
3. **Phase `duration_s` minimum is 0.2 s, not 1 s**, because legacy pulse profiles use on/off times down to 200 ms.
4. **Utility `max_sec` goes up to 120**, because Backflush runs 100 s.
5. **Profile temperature range is 80–100 °C.** The spec referred to "the existing brew-target clamp", but none exists.
6. **The built-in "Lever (Cremina-style)" is named "Lever"**, because the full name is 21 chars and the limit is 20.
7. **The 9-bar step test allows 10 s**, not 5, since the headspace fill takes about 3 s at full duty.
8. **When mains returns mid-shot, the pump resumes by itself.** The planner re-commands duty every tick, so it is the "new command". The gate stays low while mains is absent.
9. **The shot chart shows est. bar, the raw sensor reading and the dashed target.** Duty stays in its stat tile rather than on the chart.

## Review Focus

These are failure modes the spec implies but the per-task unit tests don't fully cover. Each one has a pinning test or check in the task named.

1. **A corrupt or half-written `/profiles.json`, or a garbage legacy file.** Boot must continue with built-ins and must never crash or reformat. A corrupt store is moved to `/profiles.json.bad`. *(Task 3: `test_migrate_garbage_returns_zero`; Task 7 Step 9 bench check.)*
2. **Name collisions and hostile names.** That includes a case-insensitive duplicate, a migrated legacy profile named like a built-in, and names containing quotes or `<script>`. They must be rejected or renamed, and rendered as text. *(Task 3: `test_name_equals_ignores_case`, `test_migrate_renames_builtin_collision`; Task 7 bench check with a `"<b>"` name.)*
3. **Mains lost mid-calibration or mid-shot.** The gate goes low, and a calibration run is discarded, never saved as a short measurement. *(Task 2: `test_zero_cycles_rejected`; Task 12 live steps 2 and 3.)*
4. **The active profile is deleted, renamed or missing at boot.** The active selection falls back to "Classic 9 bar" and is never left dangling. *(Task 7 Step 9 bench check.)*
5. **Junk numbers from the UI** (NaN from `toFloat("abc")`, 0, huge values) in tweak or calibration inputs. These must be clamped into a valid profile and must never produce an unrunnable one. *(Task 3: `test_tweak_clamps_nan_and_extremes`; Task 2: `test_grams_nonpositive_rejected`.)*

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `platformio.ini` | modify | add `[env:native]` |
| `include/config.h` | modify | new model/profile/dimmer/calibration constants; remove the obsolete pre-infusion/pressure-stage/auto-stop defines |
| `include/pump_model.h`, `src/pump_model.cpp` | create | pure EP5 + reference-puck model, duty laws, `P_est` integration |
| `include/pump_cal_math.h`, `src/pump_cal_math.cpp` | create | pure calibration arithmetic (per-run ml/click, plausibility, outlier) |
| `include/profile_types.h` | create | `Phase` / `Profile` / `ProfileTweak` structs and enums |
| `include/profile_codec.h`, `src/profile_codec.cpp` | create | pure JSON ⇄ struct, validation, built-in table, legacy migration, tweak clamping |
| `include/shot_planner.h`, `src/shot_planner.cpp` | create | pure phase sequencer + curves |
| `include/dimmer.h`, `src/dimmer.cpp` | modify | long gate via gptimer, mains detect/watchdog, counters, slew, clean start |
| `include/pump_calibration.h`, `src/pump_calibration.cpp` | create | device-side calibration runner + NVS; owns `pumpMlPerClick` |
| `include/profiles.h`, `src/profiles.cpp` | rewrite | LittleFS store, overrides, migration + backup, mutations by name |
| `src/main.cpp` | modify | planner integration, profile selection by name, history buffers, remove ShotStage |
| `src/web.cpp` | modify | `/status` / `/update` / `/profiles` / export API; Now, Profiles and Settings UI |
| `test/test_pump_model/test_main.cpp` | create | model tests |
| `test/test_pump_cal_math/test_main.cpp` | create | calibration maths tests |
| `test/test_profile_codec/test_main.cpp` | create | codec, built-in, migration and tweak tests |
| `test/test_shot_planner/test_main.cpp` | create | planner and curve tests |
| `AGENTS.md`, `HARDWARE_ROADMAP.md` | modify | docs and change log |

---

### Task 1: Native test toolchain + pump model

**Files:**
- Modify: `platformio.ini` (append an env)
- Modify: `include/config.h` (append a section after the `PRESSURE_CLOSED_LOOP_AVAILABLE` define)
- Create: `include/pump_model.h`, `src/pump_model.cpp`
- Test: `test/test_pump_model/test_main.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces (used by Tasks 4, 6 and 7):
  ```cpp
  struct PumpModelParams { float mlPerClick; float cps; float trimPct; };
  struct PumpModelState { float pressureBar; float mlPumped; };
  float pumpShape(float bar);
  float pumpFlowFull(const PumpModelParams &m, float bar);
  float puckFlow(const PumpModelParams &m, float bar);
  float dutyForPressure(const PumpModelParams &m, float pressureNow, float targetBar); // 0..1
  float dutyForFlow(const PumpModelParams &m, float pressureNow, float flowMlS);       // 0..1
  void pumpModelReset(PumpModelState &s);
  void pumpModelStep(const PumpModelParams &m, PumpModelState &s, float duty, float dtS);
  ```
  Constants in `config.h`: `PUMP_ML_PER_CLICK_DEFAULT`, `PUMP_ML_PER_CLICK_MIN`, `PUMP_ML_PER_CLICK_MAX`, `PUMP_MODEL_*`.

- [ ] **Step 1: Make sure a host C++ compiler exists**

Run (PowerShell): `gcc --version`
- If it prints a version, go to Step 2.
- If it is "not recognized", install WinLibs (ask the user before installing system software):
  `winget install --id BrechtSanders.WinLibs.POSIX.UCRT --exact --accept-package-agreements --accept-source-agreements`
  Then open a **new** shell and re-run `gcc --version`. It should print `gcc.exe (MinGW-W64 ...)`.
- **Fallback if installing isn't possible:** run the same tests on the board over USB. Add the env below to `platformio.ini` and use `& $pio test -e esp32-s3-test` wherever this plan says `& $pio test -e native`. The test files below compile for both.
  ```ini
  [env:esp32-s3-test]
  platform = https://github.com/pioarduino/platform-espressif32.git
  board = esp32-s3-devkitc-1
  board_build.mcu = esp32s3
  board_build.flash_size = 16MB
  board_upload.flash_size = 16MB
  board_build.arduino.memory_type = qio_opi
  framework = arduino
  test_framework = unity
  build_flags = -D ARDUINO_USB_MODE=1 -D ARDUINO_USB_CDC_ON_BOOT=1
  build_src_filter = -<*> +<pump_model.cpp> +<pump_cal_math.cpp> +<profile_codec.cpp> +<shot_planner.cpp>
  test_build_src = yes
  lib_deps = bblanchon/ArduinoJson @ ^7.2.0
  ```

- [ ] **Step 2: Add the native env**

Append to `platformio.ini`:
```ini

; --- Host unit tests for the pure control units (no Arduino, no board) ---
; Run: pio test -e native. Only the pure units are compiled - see the
; implementation plan 2026-10-02 "Global Constraints".
[env:native]
platform = native
test_framework = unity
build_flags = -std=gnu++17 -Wall
build_src_filter = -<*> +<pump_model.cpp> +<pump_cal_math.cpp> +<profile_codec.cpp> +<shot_planner.cpp>
test_build_src = yes
lib_deps = bblanchon/ArduinoJson @ ^7.2.0
```

- [ ] **Step 3: Add the model constants to `config.h`**

Insert directly after the line `#define PRESSURE_CLOSED_LOOP_AVAILABLE false`:
```cpp

// ============================================================================
// Open-loop pump model (design spec 2026-10-02 §4.1)
// ----------------------------------------------------------------------------
// Pump side: one stroke per mains cycle, ml per stroke falls with pressure
// along Gaggiuino's EP5 curve SHAPE (pump_model.cpp), scaled to OUR pump by
// the 3-run scale calibration (pump_calibration.cpp). 0.213 ml/stroke is
// Gaggiuino's effective free-flow value (their pumpFlowAtZero 0.2225 minus
// the 0.045 curve offset, x1.2 multiplier at 50 Hz) - used until calibrated.
// Puck side: a "reference puck" passing PUMP_MODEL_REF_PUCK_FLOW_ML_S at
// PUMP_MODEL_REF_PUCK_BAR (a dialed 18 g -> 36 g shot), linear in pressure.
// Headspace: the estimate holds 0 bar until this much water has gone in
// (basket headspace + dry-puck absorption) - otherwise a fill phase would
// "reach" its bar limit in ~1 s and starve the fill.
// ============================================================================
#define PUMP_ML_PER_CLICK_DEFAULT 0.213f
#define PUMP_ML_PER_CLICK_MIN 0.15f
#define PUMP_ML_PER_CLICK_MAX 0.40f
#define PUMP_MODEL_COMPLIANCE_ML_PER_BAR 1.4f
#define PUMP_MODEL_TAU_S 0.5f
#define PUMP_MODEL_REF_PUCK_FLOW_ML_S 2.0f
#define PUMP_MODEL_REF_PUCK_BAR 9.0f
#define PUMP_MODEL_HEADSPACE_ML 30.0f
#define PUMP_MODEL_SHAPE_FLOOR 0.02f
#define PUMP_MODEL_MAX_BAR 16.0f
```

- [ ] **Step 4: Write the failing tests**

Create `test/test_pump_model/test_main.cpp`:
```cpp
#include <math.h>
#include <unity.h>

#include "config.h"
#include "pump_model.h"

static PumpModelParams defaults() { return {PUMP_ML_PER_CLICK_DEFAULT, 50.0f, 0.0f}; }

void setUp() {}
void tearDown() {}

void test_shape_is_one_at_zero() { TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, pumpShape(0.0f)); }

void test_shape_matches_gaggiuino_curve() {
  // Gaggiuino EP5 ml/click: 0.213 @0, 0.108 @4, 0.053 @9 bar.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.506f, pumpShape(4.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.005f, 0.2486f, pumpShape(9.0f));
}

void test_shape_floor_above_eleven_bar() {
  TEST_ASSERT_EQUAL_FLOAT(PUMP_MODEL_SHAPE_FLOOR, pumpShape(12.0f));
  TEST_ASSERT_TRUE(pumpShape(11.0f) > 0.0f);
}

void test_shape_monotonic_decreasing() {
  float prev = 2.0f;
  for (float p = 0.0f; p <= 16.0f; p += 0.25f) {
    float s = pumpShape(p);
    TEST_ASSERT_TRUE(s <= prev + 1e-6f);
    prev = s;
  }
}

void test_full_flow_at_zero_bar() {
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f * 0.213f, pumpFlowFull(defaults(), 0.0f));
}

void test_hold_duty_at_nine_bar() {
  // 2.0 / (50 * 0.213 * 0.2486) = 0.755
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.755f, dutyForPressure(defaults(), 9.0f, 9.0f));
}

void test_positive_trim_raises_duty() {
  PumpModelParams t = defaults();
  t.trimPct = 20.0f;
  TEST_ASSERT_TRUE(dutyForPressure(t, 9.0f, 9.0f) > dutyForPressure(defaults(), 9.0f, 9.0f));
}

void test_duty_always_clamped() {
  for (float p = 0.0f; p <= 16.0f; p += 0.5f) {
    for (float t = 0.0f; t <= 16.0f; t += 0.5f) {
      float d = dutyForPressure(defaults(), p, t);
      TEST_ASSERT_TRUE(d >= 0.0f && d <= 1.0f);
    }
    for (float f = 0.0f; f <= 20.0f; f += 1.0f) {
      float d = dutyForFlow(defaults(), p, f);
      TEST_ASSERT_TRUE(d >= 0.0f && d <= 1.0f);
    }
  }
}

void test_no_nan_at_high_pressure() {
  float d = dutyForPressure(defaults(), 15.0f, 11.0f);
  TEST_ASSERT_FALSE(isnan(d));
  TEST_ASSERT_TRUE(d >= 0.0f && d <= 1.0f);
}

void test_headspace_holds_estimate_at_zero() {
  PumpModelState s;
  pumpModelReset(s);
  for (int i = 0; i < 20; i++) pumpModelStep(defaults(), s, 1.0f, 0.05f); // 1 s, ~10.65 ml
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.pressureBar);
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 10.65f, s.mlPumped);
}

void test_step_to_nine_bar_settles_without_overshoot() {
  PumpModelParams m = defaults();
  PumpModelState s;
  pumpModelReset(s);
  float maxP = 0.0f;
  for (int i = 0; i < 200; i++) { // 10 s at 50 ms
    float d = dutyForPressure(m, s.pressureBar, 9.0f);
    pumpModelStep(m, s, d, 0.05f);
    if (s.pressureBar > maxP) maxP = s.pressureBar;
  }
  TEST_ASSERT_TRUE(maxP <= 9.5f);
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 9.0f, s.pressureBar);
}

void test_lower_target_lowers_duty() {
  float hold = dutyForPressure(defaults(), 9.0f, 9.0f);
  float decline = dutyForPressure(defaults(), 9.0f, 8.5f);
  TEST_ASSERT_TRUE(decline < hold);
}

void test_soak_decays_pressure() {
  PumpModelState s = {9.0f, 100.0f};
  for (int i = 0; i < 40; i++) pumpModelStep(defaults(), s, 0.0f, 0.05f); // 2 s off
  TEST_ASSERT_TRUE(s.pressureBar < 9.0f);
  TEST_ASSERT_TRUE(s.pressureBar > 0.0f);
}

static int runAll() {
  UNITY_BEGIN();
  RUN_TEST(test_shape_is_one_at_zero);
  RUN_TEST(test_shape_matches_gaggiuino_curve);
  RUN_TEST(test_shape_floor_above_eleven_bar);
  RUN_TEST(test_shape_monotonic_decreasing);
  RUN_TEST(test_full_flow_at_zero_bar);
  RUN_TEST(test_hold_duty_at_nine_bar);
  RUN_TEST(test_positive_trim_raises_duty);
  RUN_TEST(test_duty_always_clamped);
  RUN_TEST(test_no_nan_at_high_pressure);
  RUN_TEST(test_headspace_holds_estimate_at_zero);
  RUN_TEST(test_step_to_nine_bar_settles_without_overshoot);
  RUN_TEST(test_lower_target_lowers_duty);
  RUN_TEST(test_soak_decays_pressure);
  return UNITY_END();
}

#ifdef ARDUINO
#include <Arduino.h>
void setup() { delay(2000); runAll(); }
void loop() {}
#else
int main() { return runAll(); }
#endif
```

- [ ] **Step 5: Run the tests to confirm they fail**

Run: `& $pio test -e native -f test_pump_model`
Expected: a build failure, `pump_model.h: No such file or directory`.

- [ ] **Step 6: Write the header**

Create `include/pump_model.h`:
```cpp
#pragma once

// Open-loop pump/puck model (design spec 2026-10-02 §4.1). Pure C++ - no
// Arduino headers - so it builds and unit-tests on the PC (env:native).
//
// Pump: flow = duty x strokes/s x ml-per-stroke(P). Puck: the reference
// puck's flow, linear in P, scaled by the profile's trim. The duty laws pick
// the duty that keeps the two balanced at a target (plus extra flow to
// compress the system when the target is above the estimate), and
// pumpModelStep() integrates the resulting estimated pressure P_est.

struct PumpModelParams {
  float mlPerClick; // calibrated free-flow ml per pump stroke
  float cps;        // strokes per second at 100% duty (= measured mains Hz)
  float trimPct;    // -20..+20, scales the reference puck's flow
};

struct PumpModelState {
  float pressureBar; // P_est
  float mlPumped;    // ml_est
};

// ml-per-stroke at `bar` relative to free flow: 1.0 at 0 bar, falling along
// Gaggiuino's EP5 curve, never below PUMP_MODEL_SHAPE_FLOOR.
float pumpShape(float bar);
// ml/s at 100% duty against `bar`.
float pumpFlowFull(const PumpModelParams &m, float bar);
// ml/s the reference puck passes at `bar`.
float puckFlow(const PumpModelParams &m, float bar);
// Duty (0..1) to move P from pressureNow toward targetBar with time
// constant PUMP_MODEL_TAU_S, and hold it there.
float dutyForPressure(const PumpModelParams &m, float pressureNow, float targetBar);
// Duty (0..1) to pump flowMlS against pressureNow.
float dutyForFlow(const PumpModelParams &m, float pressureNow, float flowMlS);

void pumpModelReset(PumpModelState &s);
// Advances P_est/ml_est by dtS seconds at `duty` (0..1).
void pumpModelStep(const PumpModelParams &m, PumpModelState &s, float duty, float dtS);
```

- [ ] **Step 7: Write the implementation**

Create `src/pump_model.cpp`:
```cpp
#include "pump_model.h"

#include "config.h"

// Gaggiuino EP5 flow-per-click polynomial (release/stm32-blackpill,
// src/peripherals/pump.cpp, pressureInefficiencyCoefficient), evaluated at
// their default pumpFlowAtZero. Only its SHAPE is used - normalized so
// pumpShape(0) == 1 - the absolute scale comes from our own calibration.
static const float K0 = 0.045f, K1 = 0.015f, K2 = 0.0033f, K3 = 0.000685f, K4 = 0.000045f,
                   K5 = 0.009f, K6 = -0.0018f;
static const float GAGGIUINO_F0 = 0.2225f;

static float gaggiuinoFpc(float p) {
  // (K5/P + K6) * -P^2 expanded so P = 0 is well defined.
  return -K5 * p - K6 * p * p + (GAGGIUINO_F0 - K0) - (K1 + (K2 - (K3 - K4 * p) * p) * p) * p;
}

static float clamp01(float v) {
  if (!(v >= 0.0f)) return 0.0f; // also catches NaN
  return v > 1.0f ? 1.0f : v;
}

float pumpShape(float bar) {
  if (!(bar >= 0.0f)) bar = 0.0f;
  float s = gaggiuinoFpc(bar) / gaggiuinoFpc(0.0f);
  return s < PUMP_MODEL_SHAPE_FLOOR ? PUMP_MODEL_SHAPE_FLOOR : s;
}

float pumpFlowFull(const PumpModelParams &m, float bar) {
  return m.cps * m.mlPerClick * pumpShape(bar);
}

float puckFlow(const PumpModelParams &m, float bar) {
  if (!(bar > 0.0f)) return 0.0f;
  return PUMP_MODEL_REF_PUCK_FLOW_ML_S * (1.0f + m.trimPct / 100.0f) * bar / PUMP_MODEL_REF_PUCK_BAR;
}

float dutyForPressure(const PumpModelParams &m, float pressureNow, float targetBar) {
  float q = pumpFlowFull(m, pressureNow);
  if (!(q > 0.0f)) return 0.0f;
  float need = puckFlow(m, pressureNow) +
               PUMP_MODEL_COMPLIANCE_ML_PER_BAR * (targetBar - pressureNow) / PUMP_MODEL_TAU_S;
  return clamp01(need / q);
}

float dutyForFlow(const PumpModelParams &m, float pressureNow, float flowMlS) {
  float q = pumpFlowFull(m, pressureNow);
  if (!(q > 0.0f) || !(flowMlS > 0.0f)) return 0.0f;
  return clamp01(flowMlS / q);
}

void pumpModelReset(PumpModelState &s) {
  s.pressureBar = 0.0f;
  s.mlPumped = 0.0f;
}

void pumpModelStep(const PumpModelParams &m, PumpModelState &s, float duty, float dtS) {
  if (!(dtS > 0.0f)) return;
  float flow = clamp01(duty) * pumpFlowFull(m, s.pressureBar);
  s.mlPumped += flow * dtS;
  if (s.mlPumped < PUMP_MODEL_HEADSPACE_ML) {
    s.pressureBar = 0.0f; // still filling the headspace / wetting the puck
    return;
  }
  s.pressureBar += (flow - puckFlow(m, s.pressureBar)) / PUMP_MODEL_COMPLIANCE_ML_PER_BAR * dtS;
  if (s.pressureBar < 0.0f) s.pressureBar = 0.0f;
  if (s.pressureBar > PUMP_MODEL_MAX_BAR) s.pressureBar = PUMP_MODEL_MAX_BAR;
}
```

- [ ] **Step 8: Run the tests to confirm they pass**

Run: `& $pio test -e native -f test_pump_model`
Expected: `13 Tests 0 Failures 0 Ignored`, `PASSED`.

- [ ] **Step 9: Make sure the device build still passes**

Run: `& $pio run -e esp32-s3-devkitc-1`
Expected: `SUCCESS`. `pump_model.cpp` compiles into the firmware but nothing calls it yet.

- [ ] **Step 10: Commit**

```powershell
git add platformio.ini include/config.h include/pump_model.h src/pump_model.cpp test/test_pump_model
git commit -m "Add open-loop pump model and native unit-test env`n`nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Calibration maths

**Files:**
- Modify: `include/config.h` (append after the pump-model block from Task 1)
- Create: `include/pump_cal_math.h`, `src/pump_cal_math.cpp`
- Test: `test/test_pump_cal_math/test_main.cpp`

**Interfaces:**
- Consumes: `PUMP_ML_PER_CLICK_MIN` / `MAX` (Task 1).
- Produces (used by Task 6):
  ```cpp
  #define PUMP_CAL_RUNS 3
  struct CalEvaluation { float mean; int outlier; int implausible; }; // -1 = none
  float calRunMlPerClick(float grams, uint32_t firedCycles); // < 0 on bad input
  bool calPlausible(float mlPerClick);
  CalEvaluation calEvaluate(const float runs[PUMP_CAL_RUNS]);
  ```
  Config: `PUMP_CAL_RUN_MS`, `PUMP_CAL_MAX_SPREAD`.

- [ ] **Step 1: Add the constants**

Append after the pump-model block in `config.h`:
```cpp
// Pump calibration (design spec 2026-10-02 §6): 3 free-flow runs at 100%,
// averaged; a run more than PUMP_CAL_MAX_SPREAD from the 3-run mean must be
// redone.
#define PUMP_CAL_RUN_MS 10000UL
#define PUMP_CAL_MAX_SPREAD 0.10f
```

- [ ] **Step 2: Write the failing tests**

Create `test/test_pump_cal_math/test_main.cpp`:
```cpp
#include <unity.h>

#include "config.h"
#include "pump_cal_math.h"

void setUp() {}
void tearDown() {}

void test_run_value_is_grams_per_cycle() {
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.2f, calRunMlPerClick(100.0f, 500));
}

void test_zero_cycles_rejected() { TEST_ASSERT_TRUE(calRunMlPerClick(100.0f, 0) < 0.0f); }

void test_grams_nonpositive_rejected() {
  TEST_ASSERT_TRUE(calRunMlPerClick(0.0f, 500) < 0.0f);
  TEST_ASSERT_TRUE(calRunMlPerClick(-5.0f, 500) < 0.0f);
}

void test_plausible_bounds() {
  TEST_ASSERT_TRUE(calPlausible(0.15f));
  TEST_ASSERT_TRUE(calPlausible(0.40f));
  TEST_ASSERT_FALSE(calPlausible(0.149f));
  TEST_ASSERT_FALSE(calPlausible(0.41f));
  TEST_ASSERT_FALSE(calPlausible(-1.0f));
}

void test_mean_and_no_outlier_when_close() {
  float r[3] = {0.20f, 0.21f, 0.22f};
  CalEvaluation e = calEvaluate(r);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.21f, e.mean);
  TEST_ASSERT_EQUAL_INT(-1, e.outlier);
  TEST_ASSERT_EQUAL_INT(-1, e.implausible);
}

void test_outlier_is_the_worst_run() {
  float r[3] = {0.20f, 0.20f, 0.26f}; // mean 0.22; run 3 is 18% off
  CalEvaluation e = calEvaluate(r);
  TEST_ASSERT_EQUAL_INT(2, e.outlier);
}

void test_implausible_run_reported() {
  float r[3] = {0.20f, 0.05f, 0.21f};
  CalEvaluation e = calEvaluate(r);
  TEST_ASSERT_EQUAL_INT(1, e.implausible);
}

static int runAll() {
  UNITY_BEGIN();
  RUN_TEST(test_run_value_is_grams_per_cycle);
  RUN_TEST(test_zero_cycles_rejected);
  RUN_TEST(test_grams_nonpositive_rejected);
  RUN_TEST(test_plausible_bounds);
  RUN_TEST(test_mean_and_no_outlier_when_close);
  RUN_TEST(test_outlier_is_the_worst_run);
  RUN_TEST(test_implausible_run_reported);
  return UNITY_END();
}

#ifdef ARDUINO
#include <Arduino.h>
void setup() { delay(2000); runAll(); }
void loop() {}
#else
int main() { return runAll(); }
#endif
```

- [ ] **Step 3: Run the tests to confirm they fail**

Run: `& $pio test -e native -f test_pump_cal_math`
Expected: a build failure, `pump_cal_math.h: No such file or directory`.

- [ ] **Step 4: Write the header and the implementation**

Create `include/pump_cal_math.h`:
```cpp
#pragma once

#include <stdint.h>

// Pure arithmetic for the 3-run pump calibration (design spec 2026-10-02
// §6). The run state machine itself is device-side (pump_calibration.cpp).

#define PUMP_CAL_RUNS 3

struct CalEvaluation {
  float mean;      // mean ml/stroke of the runs
  int outlier;     // index of the run furthest from the mean if > PUMP_CAL_MAX_SPREAD off, else -1
  int implausible; // first run outside PUMP_ML_PER_CLICK_MIN..MAX, else -1
};

// ml per pump stroke for one run; negative if the input can't be a real run.
float calRunMlPerClick(float grams, uint32_t firedCycles);
bool calPlausible(float mlPerClick);
CalEvaluation calEvaluate(const float runs[PUMP_CAL_RUNS]);
```

Create `src/pump_cal_math.cpp`:
```cpp
#include "pump_cal_math.h"

#include <math.h>

#include "config.h"

float calRunMlPerClick(float grams, uint32_t firedCycles) {
  if (firedCycles == 0 || !(grams > 0.0f)) return -1.0f;
  return grams / (float)firedCycles;
}

bool calPlausible(float mlPerClick) {
  return mlPerClick >= PUMP_ML_PER_CLICK_MIN && mlPerClick <= PUMP_ML_PER_CLICK_MAX;
}

CalEvaluation calEvaluate(const float runs[PUMP_CAL_RUNS]) {
  CalEvaluation e = {0.0f, -1, -1};
  float sum = 0.0f;
  for (int i = 0; i < PUMP_CAL_RUNS; i++) {
    if (e.implausible < 0 && !calPlausible(runs[i])) e.implausible = i;
    sum += runs[i];
  }
  e.mean = sum / PUMP_CAL_RUNS;
  if (!(e.mean > 0.0f)) return e;
  float worst = PUMP_CAL_MAX_SPREAD;
  for (int i = 0; i < PUMP_CAL_RUNS; i++) {
    float dev = fabsf(runs[i] - e.mean) / e.mean;
    if (dev > worst) {
      worst = dev;
      e.outlier = i;
    }
  }
  return e;
}
```

- [ ] **Step 5: Run the tests to confirm they pass**

Run: `& $pio test -e native -f test_pump_cal_math`
Expected: `7 Tests 0 Failures`, `PASSED`.

- [ ] **Step 6: Commit**

```powershell
git add include/config.h include/pump_cal_math.h src/pump_cal_math.cpp test/test_pump_cal_math
git commit -m "Add pump calibration maths (per-run ml/stroke, plausibility, outlier)`n`nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Profile types, codec, built-ins, migration

**Files:**
- Modify: `include/config.h` (after the `#define PROFILE_LOG_PATH "/profiles.csv"` line)
- Create: `include/profile_types.h`, `include/profile_codec.h`, `src/profile_codec.cpp`
- Test: `test/test_profile_codec/test_main.cpp`

**Interfaces:**
- Consumes: `PROFILE_NAME_MAX_LEN` (20) and `PROFILE_MAX_COUNT` (8), both already in `config.h`.
- Produces (used by Tasks 4, 7 and 9):
  ```cpp
  enum class PhaseType : uint8_t { PRESSURE, FLOW, POWER, SOAK };
  enum class Curve : uint8_t { INSTANT, LINEAR, EASE_IN, EASE_OUT, EASE_IN_OUT };
  struct Phase { char name[PHASE_NAME_MAX_LEN + 1]; PhaseType type; float target; float limit;
                 bool hasStart; float start; Curve curve; float rampS; float durationS;
                 float stopPressGte; float stopPressLte; };   // stop < 0 = none
  struct Profile { char name[PROFILE_NAME_MAX_LEN + 1]; bool builtin; bool utility; float temp;
                   uint16_t maxSec; float pressureScale; int8_t trimPct; uint8_t phaseCount;
                   Phase phases[PROFILE_UTILITY_PHASES_MAX]; };
  struct ProfileTweak { float temp; uint16_t maxSec; float pressureScale; int8_t trimPct; };
  struct LegacyPowers { int brewPct; int preinfusionPct; };
  bool profileFromJson(JsonObjectConst obj, Profile &out, const char **err);
  void profileToJson(const Profile &p, JsonObject obj);
  bool profileValidate(const Profile &p, const char **err);
  int builtinProfilesLoad(Profile *out, int maxOut);
  ProfileTweak profileGetTweak(const Profile &p);
  void profileApplyTweak(Profile &p, const ProfileTweak &t); // clamps every field
  bool profileNameEquals(const char *a, const char *b);      // case-insensitive
  void profileMakeOldName(const char *in, char *out, size_t cap); // "<name> (old)", truncated to 20
  int migrateLegacyProfiles(const char *legacyJson, const LegacyPowers &powers,
                            const Profile *taken, int takenCount,
                            Profile *out, int maxOut, int *warnings);
  const char *phaseTypeName(PhaseType t); // "pressure"/"flow"/"power"/"soak"
  ```

- [ ] **Step 1: Add the constants**

Insert after `#define PROFILE_LOG_PATH "/profiles.csv"` in `config.h`:
```cpp

// Phase-based profiles (design spec 2026-10-02 §3). PROFILE_MAX_COUNT above
// is now the USER-profile limit; built-ins live in firmware
// (profile_codec.cpp) and don't count against it. PROFILE_LOG_PATH is the
// LEGACY file, read once for migration, then left alone (with a byte-for-
// byte backup at PROFILE_LEGACY_BACKUP_PATH).
#define PROFILE_PHASES_MAX 8
#define PROFILE_UTILITY_PHASES_MAX 10
#define PHASE_NAME_MAX_LEN 15
#define PROFILE_BUILTIN_MAX 8
#define PROFILE_STORE_PATH "/profiles.json"
#define PROFILE_LEGACY_BACKUP_PATH "/profiles.csv.bak"
#define PROFILE_DEFAULT_NAME "Classic 9 bar"
#define PROFILE_TEMP_MIN 80.0f
#define PROFILE_TEMP_MAX 100.0f
#define PROFILE_MAX_SEC_MIN 5
#define PROFILE_MAX_SEC_MAX 90
#define PROFILE_UTILITY_MAX_SEC_MAX 120
#define PROFILE_PRESSURE_SCALE_MIN 0.7f
#define PROFILE_PRESSURE_SCALE_MAX 1.1f
#define PROFILE_TRIM_MIN -20
#define PROFILE_TRIM_MAX 20
#define PHASE_DURATION_MIN_S 0.2f
#define PHASE_DURATION_MAX_S 120.0f
#define PHASE_PRESSURE_MAX_BAR 11.0f
#define PHASE_FLOW_MAX_ML_S 10.0f
```

- [ ] **Step 2: Create `include/profile_types.h`**

```cpp
#pragma once

#include <stdint.h>

#include "config.h"

// Phase-list shot profiles (design spec 2026-10-02 §3.2). Plain structs -
// shared by the pure codec/planner and the device-side store.

enum class PhaseType : uint8_t { PRESSURE, FLOW, POWER, SOAK };
enum class Curve : uint8_t { INSTANT, LINEAR, EASE_IN, EASE_OUT, EASE_IN_OUT };

struct Phase {
  char name[PHASE_NAME_MAX_LEN + 1];
  PhaseType type;
  float target;       // bar (PRESSURE), ml/s (FLOW), % (POWER); ignored for SOAK
  float limit;        // PRESSURE: max ml/s; FLOW: max bar; 0 = none
  bool hasStart;      // explicit ramp start value (built-ins only for now)
  float start;
  Curve curve;
  float rampS;        // seconds to move from the start value to target
  float durationS;    // hard cap; the only exit without a usable sensor
  float stopPressGte; // < 0 = none; only evaluated with a usable sensor
  float stopPressLte; // < 0 = none
};

struct Profile {
  char name[PROFILE_NAME_MAX_LEN + 1];
  bool builtin;
  bool utility;        // Flush/Backflush: no temp setpoint, not logged
  float temp;
  uint16_t maxSec;
  float pressureScale; // multiplies pressure targets, flow-phase bar limits, press_* stops
  int8_t trimPct;      // scales the reference puck (pump_model.h)
  uint8_t phaseCount;
  Phase phases[PROFILE_UTILITY_PHASES_MAX];
};

// The user-editable subset (allowed on built-ins too, stored as overrides).
struct ProfileTweak {
  float temp;
  uint16_t maxSec;
  float pressureScale;
  int8_t trimPct;
};
```

- [ ] **Step 3: Write the failing tests**

Create `test/test_profile_codec/test_main.cpp`:
```cpp
#include <ArduinoJson.h>
#include <math.h>
#include <string.h>
#include <unity.h>

#include "profile_codec.h"

static Profile builtins[PROFILE_BUILTIN_MAX];
static int builtinCount = 0;

void setUp() { builtinCount = builtinProfilesLoad(builtins, PROFILE_BUILTIN_MAX); }
void tearDown() {}

static const Profile *findBuiltin(const char *name) {
  for (int i = 0; i < builtinCount; i++)
    if (strcmp(builtins[i].name, name) == 0) return &builtins[i];
  return nullptr;
}

static bool parseOne(const char *json, Profile &out, const char **err) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) { *err = "bad json"; return false; }
  return profileFromJson(doc.as<JsonObjectConst>(), out, err);
}

void test_builtins_all_load_and_validate() {
  TEST_ASSERT_EQUAL_INT(7, builtinCount);
  const char *err = nullptr;
  for (int i = 0; i < builtinCount; i++) {
    TEST_ASSERT_TRUE_MESSAGE(profileValidate(builtins[i], &err), builtins[i].name);
    TEST_ASSERT_TRUE(builtins[i].builtin);
  }
  TEST_ASSERT_NOT_NULL(findBuiltin(PROFILE_DEFAULT_NAME));
  TEST_ASSERT_NOT_NULL(findBuiltin("Londinium"));
  TEST_ASSERT_NOT_NULL(findBuiltin("Lever"));
  TEST_ASSERT_NOT_NULL(findBuiltin("IUIUIU Classic"));
  TEST_ASSERT_NOT_NULL(findBuiltin("Blooming"));
}

void test_classic_shape() {
  const Profile *p = findBuiltin(PROFILE_DEFAULT_NAME);
  TEST_ASSERT_EQUAL_INT(3, p->phaseCount);
  TEST_ASSERT_EQUAL_INT((int)PhaseType::FLOW, (int)p->phases[0].type);
  TEST_ASSERT_EQUAL_INT((int)PhaseType::PRESSURE, (int)p->phases[2].type);
  TEST_ASSERT_EQUAL_FLOAT(9.0f, p->phases[2].target);
  TEST_ASSERT_EQUAL_FLOAT(4.0f, p->phases[0].stopPressGte);
  TEST_ASSERT_TRUE(p->phases[1].stopPressGte < 0.0f);
}

void test_backflush_is_ten_phase_utility() {
  const Profile *p = findBuiltin("Backflush");
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_TRUE(p->utility);
  TEST_ASSERT_EQUAL_INT(10, p->phaseCount);
  TEST_ASSERT_EQUAL_INT(100, p->maxSec);
}

void test_iuiuiu_has_explicit_start() {
  const Profile *p = findBuiltin("IUIUIU Classic");
  const Phase &last = p->phases[p->phaseCount - 1];
  TEST_ASSERT_TRUE(last.hasStart);
  TEST_ASSERT_EQUAL_FLOAT(2.5f, last.start);
}

void test_rejects_too_many_phases_for_non_utility() {
  Profile p = *findBuiltin(PROFILE_DEFAULT_NAME);
  p.phaseCount = 9;
  for (int i = 3; i < 9; i++) p.phases[i] = p.phases[2];
  const char *err = nullptr;
  TEST_ASSERT_FALSE(profileValidate(p, &err));
  TEST_ASSERT_NOT_NULL(err);
}

void test_rejects_unknown_type() {
  Profile p;
  const char *err = nullptr;
  TEST_ASSERT_FALSE(parseOne(
      "{\"name\":\"X\",\"temp\":93,\"max_sec\":30,\"phases\":[{\"type\":\"turbo\",\"target\":1,\"duration_s\":5}]}",
      p, &err));
}

void test_rejects_pressure_over_eleven() {
  Profile p;
  const char *err = nullptr;
  TEST_ASSERT_FALSE(parseOne(
      "{\"name\":\"X\",\"temp\":93,\"max_sec\":30,\"phases\":[{\"type\":\"pressure\",\"target\":12,\"duration_s\":5}]}",
      p, &err));
}

void test_rejects_trim_out_of_range() {
  Profile p;
  const char *err = nullptr;
  TEST_ASSERT_FALSE(parseOne(
      "{\"name\":\"X\",\"temp\":93,\"max_sec\":30,\"trim_pct\":30,\"phases\":[{\"type\":\"soak\",\"duration_s\":5}]}",
      p, &err));
}

void test_round_trip_keeps_fields() {
  const Profile *src = findBuiltin("Londinium");
  JsonDocument doc;
  profileToJson(*src, doc.to<JsonObject>());
  Profile back;
  const char *err = nullptr;
  TEST_ASSERT_TRUE(profileFromJson(doc.as<JsonObjectConst>(), back, &err));
  TEST_ASSERT_EQUAL_STRING(src->name, back.name);
  TEST_ASSERT_EQUAL_INT(src->phaseCount, back.phaseCount);
  for (int i = 0; i < src->phaseCount; i++) {
    TEST_ASSERT_EQUAL_INT((int)src->phases[i].type, (int)back.phases[i].type);
    TEST_ASSERT_EQUAL_INT((int)src->phases[i].curve, (int)back.phases[i].curve);
    TEST_ASSERT_EQUAL_FLOAT(src->phases[i].target, back.phases[i].target);
    TEST_ASSERT_EQUAL_FLOAT(src->phases[i].limit, back.phases[i].limit);
    TEST_ASSERT_EQUAL_FLOAT(src->phases[i].durationS, back.phases[i].durationS);
    TEST_ASSERT_EQUAL_FLOAT(src->phases[i].stopPressGte, back.phases[i].stopPressGte);
  }
}

// The 4 profiles actually on the device (AGENTS.md, 2026-08-23 recovery).
static const char *LEGACY_FIXTURE =
    "[{\"name\":\"Classic\",\"temp\":93,\"auto_stop_sec\":27,\"preinfusion\":false,\"pulses\":0,\"on_ms\":0,\"off_ms\":0,"
    "\"pressure_enabled\":false,\"pressure_ramp_bar\":0,\"pressure_ramp_ms\":0,\"pressure_decline_enabled\":false,"
    "\"pressure_decline_bar\":0,\"pressure_decline_ms\":0},"
    "{\"name\":\"Medium Roast\",\"temp\":93,\"auto_stop_sec\":27,\"preinfusion\":true,\"pulses\":3,\"on_ms\":1000,\"off_ms\":2000},"
    "{\"name\":\"Light Roast\",\"temp\":95,\"auto_stop_sec\":28,\"preinfusion\":true,\"pulses\":4,\"on_ms\":1000,\"off_ms\":3000},"
    "{\"name\":\"Dark Roast\",\"temp\":90,\"auto_stop_sec\":26,\"preinfusion\":true,\"pulses\":2,\"on_ms\":1000,\"off_ms\":1000}]";

void test_migrates_the_four_device_profiles() {
  Profile out[PROFILE_MAX_COUNT];
  int warnings = -1;
  int n = migrateLegacyProfiles(LEGACY_FIXTURE, {45, 40}, builtins, builtinCount, out, PROFILE_MAX_COUNT, &warnings);
  TEST_ASSERT_EQUAL_INT(4, n);
  TEST_ASSERT_EQUAL_INT(0, warnings);

  TEST_ASSERT_EQUAL_STRING("Classic", out[0].name);
  TEST_ASSERT_EQUAL_INT(1, out[0].phaseCount);
  TEST_ASSERT_EQUAL_INT((int)PhaseType::POWER, (int)out[0].phases[0].type);
  TEST_ASSERT_EQUAL_FLOAT(45.0f, out[0].phases[0].target);
  TEST_ASSERT_EQUAL_FLOAT(27.0f, out[0].phases[0].durationS);
  TEST_ASSERT_EQUAL_INT(27, out[0].maxSec);

  // Medium: 3 pulses + 2 rests + extract = 6 phases; extract = 27 - (3*1 + 2*2) = 20 s
  TEST_ASSERT_EQUAL_INT(6, out[1].phaseCount);
  TEST_ASSERT_EQUAL_FLOAT(40.0f, out[1].phases[0].target);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, out[1].phases[0].durationS);
  TEST_ASSERT_EQUAL_INT((int)PhaseType::SOAK, (int)out[1].phases[1].type);
  TEST_ASSERT_EQUAL_FLOAT(2.0f, out[1].phases[1].durationS);
  TEST_ASSERT_EQUAL_FLOAT(20.0f, out[1].phases[5].durationS);
  TEST_ASSERT_EQUAL_FLOAT(93.0f, out[1].temp);
  TEST_ASSERT_FALSE(out[1].builtin);
}

void test_migrate_renames_builtin_collision() {
  Profile out[PROFILE_MAX_COUNT];
  int warnings = 0;
  int n = migrateLegacyProfiles("[{\"name\":\"londinium\",\"temp\":92,\"auto_stop_sec\":30}]", {45, 40}, builtins,
                                builtinCount, out, PROFILE_MAX_COUNT, &warnings);
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_EQUAL_STRING("londinium (old)", out[0].name);
}

void test_migrate_caps_pulses_and_warns() {
  Profile out[PROFILE_MAX_COUNT];
  int warnings = 0;
  int n = migrateLegacyProfiles(
      "[{\"name\":\"Many\",\"temp\":93,\"auto_stop_sec\":40,\"preinfusion\":true,\"pulses\":10,\"on_ms\":500,\"off_ms\":500}]",
      {45, 40}, builtins, builtinCount, out, PROFILE_MAX_COUNT, &warnings);
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_EQUAL_INT(1, warnings);
  TEST_ASSERT_EQUAL_INT(PROFILE_PHASES_MAX, out[0].phaseCount); // 4 pulses + 3 rests + extract
}

void test_migrate_skips_malformed_keeps_rest() {
  Profile out[PROFILE_MAX_COUNT];
  int warnings = 0;
  int n = migrateLegacyProfiles("[{\"temp\":93},{\"name\":\"Ok\",\"temp\":93,\"auto_stop_sec\":27}]", {45, 40}, builtins,
                                builtinCount, out, PROFILE_MAX_COUNT, &warnings);
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_EQUAL_INT(1, warnings);
  TEST_ASSERT_EQUAL_STRING("Ok", out[0].name);
}

void test_migrate_garbage_returns_zero() {
  Profile out[PROFILE_MAX_COUNT];
  int warnings = 0;
  TEST_ASSERT_EQUAL_INT(0, migrateLegacyProfiles("Classic,93,27\n{{{", {45, 40}, builtins, builtinCount, out,
                                                 PROFILE_MAX_COUNT, &warnings));
}

void test_name_equals_ignores_case() {
  TEST_ASSERT_TRUE(profileNameEquals("Classic 9 bar", "classic 9 BAR"));
  TEST_ASSERT_FALSE(profileNameEquals("Classic", "Classic 9 bar"));
}

void test_make_old_name_truncates_to_twenty() {
  char out[PROFILE_NAME_MAX_LEN + 1];
  profileMakeOldName("Twenty Char Name XYZ", out, sizeof out);
  TEST_ASSERT_EQUAL_STRING("Twenty Char Na (old)", out);
  TEST_ASSERT_EQUAL_INT(20, (int)strlen(out));
}

void test_tweak_clamps_nan_and_extremes() {
  Profile p = *findBuiltin(PROFILE_DEFAULT_NAME);
  ProfileTweak t = {NAN, 0, 5.0f, -100};
  profileApplyTweak(p, t);
  const char *err = nullptr;
  TEST_ASSERT_TRUE(profileValidate(p, &err));
  TEST_ASSERT_EQUAL_FLOAT(PROFILE_TEMP_MIN, p.temp);
  TEST_ASSERT_EQUAL_INT(PROFILE_MAX_SEC_MIN, p.maxSec);
  TEST_ASSERT_EQUAL_FLOAT(PROFILE_PRESSURE_SCALE_MAX, p.pressureScale);
  TEST_ASSERT_EQUAL_INT(PROFILE_TRIM_MIN, p.trimPct);
}

void test_tweak_ignores_temp_on_utility() {
  Profile p = *findBuiltin("Flush");
  float before = p.temp;
  ProfileTweak t = profileGetTweak(p);
  t.temp = 95.0f;
  profileApplyTweak(p, t);
  TEST_ASSERT_EQUAL_FLOAT(before, p.temp);
}

static int runAll() {
  UNITY_BEGIN();
  RUN_TEST(test_builtins_all_load_and_validate);
  RUN_TEST(test_classic_shape);
  RUN_TEST(test_backflush_is_ten_phase_utility);
  RUN_TEST(test_iuiuiu_has_explicit_start);
  RUN_TEST(test_rejects_too_many_phases_for_non_utility);
  RUN_TEST(test_rejects_unknown_type);
  RUN_TEST(test_rejects_pressure_over_eleven);
  RUN_TEST(test_rejects_trim_out_of_range);
  RUN_TEST(test_round_trip_keeps_fields);
  RUN_TEST(test_migrates_the_four_device_profiles);
  RUN_TEST(test_migrate_renames_builtin_collision);
  RUN_TEST(test_migrate_caps_pulses_and_warns);
  RUN_TEST(test_migrate_skips_malformed_keeps_rest);
  RUN_TEST(test_migrate_garbage_returns_zero);
  RUN_TEST(test_name_equals_ignores_case);
  RUN_TEST(test_make_old_name_truncates_to_twenty);
  RUN_TEST(test_tweak_clamps_nan_and_extremes);
  RUN_TEST(test_tweak_ignores_temp_on_utility);
  return UNITY_END();
}

#ifdef ARDUINO
#include <Arduino.h>
void setup() { delay(2000); runAll(); }
void loop() {}
#else
int main() { return runAll(); }
#endif
```

- [ ] **Step 4: Run the tests to confirm they fail**

Run: `& $pio test -e native -f test_profile_codec`
Expected: a build failure, `profile_codec.h: No such file or directory`.

- [ ] **Step 5: Create `include/profile_codec.h`**

```cpp
#pragma once

#include <ArduinoJson.h>
#include <stddef.h>

#include "profile_types.h"

// Pure JSON <-> Profile conversion, validation, the built-in profile table,
// tweak clamping and legacy migration (design spec 2026-10-02 §3). No
// Arduino/LittleFS - the device-side store (profiles.cpp) does the I/O.

// Parses + validates one profile object. On failure returns false and sets
// *err to a static human-readable reason.
bool profileFromJson(JsonObjectConst obj, Profile &out, const char **err);
void profileToJson(const Profile &p, JsonObject obj);
bool profileValidate(const Profile &p, const char **err);

// Parses the firmware's built-in profile table into `out` (builtin = true).
// Returns how many loaded.
int builtinProfilesLoad(Profile *out, int maxOut);

ProfileTweak profileGetTweak(const Profile &p);
// Applies a tweak, clamping every field into range (NaN -> the minimum).
// Temperature is ignored for utility profiles.
void profileApplyTweak(Profile &p, const ProfileTweak &t);

bool profileNameEquals(const char *a, const char *b); // case-insensitive
// "<in> (old)", with `in` truncated so the result fits PROFILE_NAME_MAX_LEN.
void profileMakeOldName(const char *in, char *out, size_t cap);

struct LegacyPowers {
  int brewPct;        // old global "pump_brew_pct"
  int preinfusionPct; // old global "pump_pi_pct"
};
// Converts the pre-2026-10-02 flat JSON array (still stored at
// PROFILE_LOG_PATH) into phase profiles. Names that collide (case-
// insensitively) with `taken` or with an earlier migrated profile get
// " (old)". Invalid entries are skipped. *warnings counts skipped entries
// and capped pulse counts. Returns the number written to `out`.
int migrateLegacyProfiles(const char *legacyJson, const LegacyPowers &powers, const Profile *taken,
                          int takenCount, Profile *out, int maxOut, int *warnings);

const char *phaseTypeName(PhaseType t);
```

- [ ] **Step 6: Create `src/profile_codec.cpp`**

```cpp
#include "profile_codec.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
// Built-in profiles (design spec 2026-10-02 §3.3) - translated from
// Gaggiuino/GaggiMate defaults so every phase also exits on its own
// duration_s (no usable sensor yet). Pressure/flow values are model targets
// (pump_model.cpp), not measurements. "stop" conditions only apply once a
// 0-16 bar sensor is fitted (PRESSURE_CLOSED_LOOP_AVAILABLE).
// ============================================================================
static const char BUILTIN_PROFILES_JSON[] = R"json([
{"name":"Classic 9 bar","temp":93,"max_sec":35,"phases":[
 {"name":"Fill","type":"flow","target":4,"limit":4,"duration_s":6,"stop":{"press_gte":4}},
 {"name":"Ramp","type":"pressure","target":9,"curve":"linear","ramp_s":3,"duration_s":3},
 {"name":"Hold","type":"pressure","target":9,"duration_s":26}]},
{"name":"Londinium","temp":92,"max_sec":45,"phases":[
 {"name":"Fill","type":"flow","target":6,"limit":4,"duration_s":8,"stop":{"press_gte":4}},
 {"name":"Soak","type":"soak","duration_s":10},
 {"name":"Rise","type":"pressure","target":9,"curve":"ease-out","ramp_s":2,"duration_s":2},
 {"name":"Hold","type":"pressure","target":9,"limit":3,"duration_s":4},
 {"name":"Decline","type":"pressure","target":3,"limit":3,"curve":"linear","ramp_s":20,"duration_s":20}]},
{"name":"Lever","temp":88,"max_sec":50,"phases":[
 {"name":"Pre-infuse","type":"pressure","target":1.5,"duration_s":8},
 {"name":"Hold","type":"pressure","target":1.5,"duration_s":7},
 {"name":"Rise","type":"pressure","target":9,"curve":"ease-in-out","ramp_s":8,"duration_s":8},
 {"name":"Decline","type":"pressure","target":3,"curve":"linear","ramp_s":25,"duration_s":25}]},
{"name":"IUIUIU Classic","temp":93,"max_sec":45,"phases":[
 {"name":"Pre-infuse","type":"flow","target":3,"limit":4,"duration_s":10,"stop":{"press_gte":4}},
 {"name":"Soak","type":"soak","duration_s":8},
 {"name":"Rise","type":"pressure","target":7.5,"curve":"ease-out","ramp_s":5,"duration_s":5},
 {"name":"Ease","type":"pressure","target":6,"limit":3,"curve":"ease-in-out","ramp_s":4,"duration_s":4},
 {"name":"Flow","type":"flow","target":1,"start":2.5,"limit":6,"curve":"ease-in-out","ramp_s":15,"duration_s":15}]},
{"name":"Blooming","temp":93,"max_sec":55,"phases":[
 {"name":"Fill","type":"flow","target":4,"limit":4,"duration_s":8,"stop":{"press_gte":4}},
 {"name":"Bloom","type":"soak","duration_s":25},
 {"name":"Ramp","type":"flow","target":2,"limit":6,"curve":"ease-out","ramp_s":5,"duration_s":5},
 {"name":"Flow","type":"flow","target":2,"limit":6,"duration_s":15}]},
{"name":"Flush","utility":true,"temp":0,"max_sec":10,"phases":[
 {"name":"Flush","type":"power","target":100,"duration_s":5}]},
{"name":"Backflush","utility":true,"temp":0,"max_sec":100,"phases":[
 {"name":"On 1","type":"power","target":100,"duration_s":10},{"name":"Off 1","type":"soak","duration_s":10},
 {"name":"On 2","type":"power","target":100,"duration_s":10},{"name":"Off 2","type":"soak","duration_s":10},
 {"name":"On 3","type":"power","target":100,"duration_s":10},{"name":"Off 3","type":"soak","duration_s":10},
 {"name":"On 4","type":"power","target":100,"duration_s":10},{"name":"Off 4","type":"soak","duration_s":10},
 {"name":"On 5","type":"power","target":100,"duration_s":10},{"name":"Off 5","type":"soak","duration_s":10}]}
])json";

static void copyStr(char *dst, size_t cap, const char *src) {
  if (!src) src = "";
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

static bool inRange(float v, float lo, float hi) { return v >= lo && v <= hi; } // false for NaN

static float clampf(float v, float lo, float hi) {
  if (!(v >= lo)) return lo; // NaN -> lo
  return v > hi ? hi : v;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

const char *phaseTypeName(PhaseType t) {
  switch (t) {
    case PhaseType::PRESSURE: return "pressure";
    case PhaseType::FLOW: return "flow";
    case PhaseType::POWER: return "power";
    default: return "soak";
  }
}

static const char *curveName(Curve c) {
  switch (c) {
    case Curve::LINEAR: return "linear";
    case Curve::EASE_IN: return "ease-in";
    case Curve::EASE_OUT: return "ease-out";
    case Curve::EASE_IN_OUT: return "ease-in-out";
    default: return "instant";
  }
}

static bool parseType(const char *s, PhaseType &t) {
  if (!s) return false;
  if (!strcmp(s, "pressure")) t = PhaseType::PRESSURE;
  else if (!strcmp(s, "flow")) t = PhaseType::FLOW;
  else if (!strcmp(s, "power")) t = PhaseType::POWER;
  else if (!strcmp(s, "soak")) t = PhaseType::SOAK;
  else return false;
  return true;
}

static bool parseCurve(const char *s, Curve &c) {
  if (!s || !strcmp(s, "instant")) c = Curve::INSTANT;
  else if (!strcmp(s, "linear")) c = Curve::LINEAR;
  else if (!strcmp(s, "ease-in")) c = Curve::EASE_IN;
  else if (!strcmp(s, "ease-out")) c = Curve::EASE_OUT;
  else if (!strcmp(s, "ease-in-out")) c = Curve::EASE_IN_OUT;
  else return false;
  return true;
}

static bool targetInRange(PhaseType t, float v) {
  switch (t) {
    case PhaseType::PRESSURE: return inRange(v, 0.0f, PHASE_PRESSURE_MAX_BAR);
    case PhaseType::FLOW: return inRange(v, 0.0f, PHASE_FLOW_MAX_ML_S);
    case PhaseType::POWER: return inRange(v, 0.0f, 100.0f);
    default: return true; // SOAK ignores its target
  }
}

bool profileValidate(const Profile &p, const char **err) {
  if (p.name[0] == '\0') { *err = "Name is empty"; return false; }
  int maxPhases = p.utility ? PROFILE_UTILITY_PHASES_MAX : PROFILE_PHASES_MAX;
  if (p.phaseCount < 1) { *err = "Profile has no phases"; return false; }
  if (p.phaseCount > maxPhases) { *err = "Too many phases"; return false; }
  if (!p.utility && !inRange(p.temp, PROFILE_TEMP_MIN, PROFILE_TEMP_MAX)) { *err = "Temperature out of range (80-100)"; return false; }
  int maxSecMax = p.utility ? PROFILE_UTILITY_MAX_SEC_MAX : PROFILE_MAX_SEC_MAX;
  if (p.maxSec < PROFILE_MAX_SEC_MIN || p.maxSec > maxSecMax) { *err = "Max shot time out of range"; return false; }
  if (!inRange(p.pressureScale, PROFILE_PRESSURE_SCALE_MIN, PROFILE_PRESSURE_SCALE_MAX)) { *err = "Pressure scale out of range (0.7-1.1)"; return false; }
  if (p.trimPct < PROFILE_TRIM_MIN || p.trimPct > PROFILE_TRIM_MAX) { *err = "Trim out of range (-20..20)"; return false; }
  for (int i = 0; i < p.phaseCount; i++) {
    const Phase &f = p.phases[i];
    if (!inRange(f.durationS, PHASE_DURATION_MIN_S, PHASE_DURATION_MAX_S)) { *err = "Phase duration out of range"; return false; }
    if (!inRange(f.rampS, 0.0f, f.durationS)) { *err = "Phase ramp longer than the phase"; return false; }
    if (!targetInRange(f.type, f.target)) { *err = "Phase target out of range"; return false; }
    if (f.hasStart && !targetInRange(f.type, f.start)) { *err = "Phase start out of range"; return false; }
    float limitMax = f.type == PhaseType::FLOW ? PHASE_PRESSURE_MAX_BAR : PHASE_FLOW_MAX_ML_S;
    if (!inRange(f.limit, 0.0f, limitMax)) { *err = "Phase limit out of range"; return false; }
    if (f.stopPressGte >= 0.0f && f.stopPressGte > PHASE_PRESSURE_MAX_BAR) { *err = "Stop pressure out of range"; return false; }
    if (f.stopPressLte >= 0.0f && f.stopPressLte > PHASE_PRESSURE_MAX_BAR) { *err = "Stop pressure out of range"; return false; }
  }
  return true;
}

bool profileFromJson(JsonObjectConst o, Profile &p, const char **err) {
  memset(&p, 0, sizeof p);
  const char *name = o["name"].as<const char *>();
  if (!name || !*name) { *err = "Missing name"; return false; }
  if (strlen(name) > PROFILE_NAME_MAX_LEN) { *err = "Name longer than 20 characters"; return false; }
  copyStr(p.name, sizeof p.name, name);
  p.utility = o["utility"] | false;
  p.temp = o["temp"] | 93.0f;
  int maxSec = o["max_sec"] | 30;
  int trim = o["trim_pct"] | 0;
  if (maxSec < 0 || maxSec > 600) { *err = "Max shot time out of range"; return false; }
  if (trim < PROFILE_TRIM_MIN || trim > PROFILE_TRIM_MAX) { *err = "Trim out of range (-20..20)"; return false; }
  p.maxSec = (uint16_t)maxSec;
  p.trimPct = (int8_t)trim;
  p.pressureScale = o["pressure_scale"] | 1.0f;

  JsonArrayConst phases = o["phases"].as<JsonArrayConst>();
  if (phases.isNull()) { *err = "Missing phases"; return false; }
  if (phases.size() > PROFILE_UTILITY_PHASES_MAX) { *err = "Too many phases"; return false; }
  for (JsonVariantConst v : phases) {
    JsonObjectConst j = v.as<JsonObjectConst>();
    Phase &f = p.phases[p.phaseCount];
    copyStr(f.name, sizeof f.name, j["name"].as<const char *>());
    if (!parseType(j["type"].as<const char *>(), f.type)) { *err = "Unknown phase type"; return false; }
    if (!parseCurve(j["curve"].as<const char *>(), f.curve)) { *err = "Unknown curve"; return false; }
    f.target = j["target"] | 0.0f;
    f.limit = j["limit"] | 0.0f;
    f.hasStart = !j["start"].isNull();
    f.start = j["start"] | 0.0f;
    f.rampS = j["ramp_s"] | 0.0f;
    f.durationS = j["duration_s"] | 0.0f;
    f.stopPressGte = j["stop"]["press_gte"] | -1.0f;
    f.stopPressLte = j["stop"]["press_lte"] | -1.0f;
    p.phaseCount++;
  }
  return profileValidate(p, err);
}

void profileToJson(const Profile &p, JsonObject o) {
  o["name"] = p.name;
  o["builtin"] = p.builtin;
  o["utility"] = p.utility;
  o["temp"] = p.temp;
  o["max_sec"] = p.maxSec;
  o["pressure_scale"] = p.pressureScale;
  o["trim_pct"] = p.trimPct;
  JsonArray phases = o["phases"].to<JsonArray>();
  for (int i = 0; i < p.phaseCount; i++) {
    const Phase &f = p.phases[i];
    JsonObject j = phases.add<JsonObject>();
    j["name"] = f.name;
    j["type"] = phaseTypeName(f.type);
    j["target"] = f.target;
    if (f.limit > 0.0f) j["limit"] = f.limit;
    if (f.hasStart) j["start"] = f.start;
    if (f.curve != Curve::INSTANT) j["curve"] = curveName(f.curve);
    if (f.rampS > 0.0f) j["ramp_s"] = f.rampS;
    j["duration_s"] = f.durationS;
    if (f.stopPressGte >= 0.0f || f.stopPressLte >= 0.0f) {
      JsonObject s = j["stop"].to<JsonObject>();
      if (f.stopPressGte >= 0.0f) s["press_gte"] = f.stopPressGte;
      if (f.stopPressLte >= 0.0f) s["press_lte"] = f.stopPressLte;
    }
  }
}

int builtinProfilesLoad(Profile *out, int maxOut) {
  JsonDocument doc;
  if (deserializeJson(doc, BUILTIN_PROFILES_JSON)) return 0;
  int n = 0;
  for (JsonVariantConst v : doc.as<JsonArrayConst>()) {
    if (n >= maxOut) break;
    const char *err = nullptr;
    if (!profileFromJson(v.as<JsonObjectConst>(), out[n], &err)) continue;
    out[n].builtin = true;
    n++;
  }
  return n;
}

ProfileTweak profileGetTweak(const Profile &p) { return {p.temp, p.maxSec, p.pressureScale, p.trimPct}; }

void profileApplyTweak(Profile &p, const ProfileTweak &t) {
  if (!p.utility) p.temp = clampf(t.temp, PROFILE_TEMP_MIN, PROFILE_TEMP_MAX);
  int maxSecMax = p.utility ? PROFILE_UTILITY_MAX_SEC_MAX : PROFILE_MAX_SEC_MAX;
  p.maxSec = (uint16_t)clampi(t.maxSec, PROFILE_MAX_SEC_MIN, maxSecMax);
  p.pressureScale = clampf(t.pressureScale, PROFILE_PRESSURE_SCALE_MIN, PROFILE_PRESSURE_SCALE_MAX);
  p.trimPct = (int8_t)clampi(t.trimPct, PROFILE_TRIM_MIN, PROFILE_TRIM_MAX);
}

bool profileNameEquals(const char *a, const char *b) {
  if (!a || !b) return false;
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    a++;
    b++;
  }
  return *a == *b;
}

void profileMakeOldName(const char *in, char *out, size_t cap) {
  static const char SUFFIX[] = " (old)";
  size_t keep = PROFILE_NAME_MAX_LEN - (sizeof SUFFIX - 1);
  char base[PROFILE_NAME_MAX_LEN + 1];
  copyStr(base, keep + 1, in);
  snprintf(out, cap, "%s%s", base, SUFFIX);
}

static bool nameTaken(const char *name, const Profile *a, int an, const Profile *b, int bn) {
  for (int i = 0; i < an; i++) if (profileNameEquals(name, a[i].name)) return true;
  for (int i = 0; i < bn; i++) if (profileNameEquals(name, b[i].name)) return true;
  return false;
}

static void addPhase(Profile &p, const char *name, PhaseType t, float target, float durS) {
  Phase &f = p.phases[p.phaseCount++];
  memset(&f, 0, sizeof f);
  copyStr(f.name, sizeof f.name, name);
  f.type = t;
  f.target = target;
  f.curve = Curve::INSTANT;
  f.durationS = clampf(durS, PHASE_DURATION_MIN_S, PHASE_DURATION_MAX_S);
  f.stopPressGte = -1.0f;
  f.stopPressLte = -1.0f;
}

int migrateLegacyProfiles(const char *legacyJson, const LegacyPowers &powers, const Profile *taken,
                          int takenCount, Profile *out, int maxOut, int *warnings) {
  *warnings = 0;
  JsonDocument doc;
  if (deserializeJson(doc, legacyJson) || !doc.is<JsonArray>()) return 0;
  int n = 0;
  for (JsonVariantConst v : doc.as<JsonArrayConst>()) {
    JsonObjectConst o = v.as<JsonObjectConst>();
    if (n >= maxOut) { (*warnings)++; break; }
    const char *name = o["name"].as<const char *>();
    if (!name || !*name) { (*warnings)++; continue; }

    Profile p;
    memset(&p, 0, sizeof p);
    copyStr(p.name, sizeof p.name, name);
    if (nameTaken(p.name, taken, takenCount, out, n)) {
      char renamed[PROFILE_NAME_MAX_LEN + 1];
      profileMakeOldName(p.name, renamed, sizeof renamed);
      copyStr(p.name, sizeof p.name, renamed);
    }
    p.temp = clampf(o["temp"] | 93.0f, PROFILE_TEMP_MIN, PROFILE_TEMP_MAX);
    int autoStop = o["auto_stop_sec"] | 27;
    p.maxSec = (uint16_t)(autoStop <= 0 ? PROFILE_MAX_SEC_MAX : clampi(autoStop, PROFILE_MAX_SEC_MIN, PROFILE_MAX_SEC_MAX));
    p.pressureScale = 1.0f;
    p.trimPct = 0;

    bool press = o["pressure_enabled"] | false;
    bool decline = press && (o["pressure_decline_enabled"] | false);
    int tail = press ? (decline ? 3 : 2) : 1;
    int pulses = (o["preinfusion"] | false) ? (int)(o["pulses"] | 0) : 0;
    int maxPulses = (PROFILE_PHASES_MAX - tail + 1) / 2;
    if (pulses > maxPulses) { pulses = maxPulses; (*warnings)++; }
    if (pulses < 0) pulses = 0;
    float onS = (float)(o["on_ms"] | 1000) / 1000.0f;
    float offS = (float)(o["off_ms"] | 1000) / 1000.0f;
    float piPct = (float)clampi(powers.preinfusionPct, 10, 100);

    float used = 0.0f;
    char label[PHASE_NAME_MAX_LEN + 1];
    for (int i = 0; i < pulses; i++) {
      snprintf(label, sizeof label, "Pulse %d", i + 1);
      addPhase(p, label, PhaseType::POWER, piPct, onS);
      used += p.phases[p.phaseCount - 1].durationS;
      if (i < pulses - 1) {
        snprintf(label, sizeof label, "Rest %d", i + 1);
        addPhase(p, label, PhaseType::SOAK, 0.0f, offS);
        used += p.phases[p.phaseCount - 1].durationS;
      }
    }
    if (press) {
      float rampBar = clampf(o["pressure_ramp_bar"] | 9.0f, 0.0f, PHASE_PRESSURE_MAX_BAR);
      addPhase(p, "Ramp", PhaseType::PRESSURE, rampBar, (float)(o["pressure_ramp_ms"] | 20000) / 1000.0f);
      used += p.phases[p.phaseCount - 1].durationS;
      float last = rampBar;
      if (decline) {
        float declineBar = clampf(o["pressure_decline_bar"] | 6.0f, 0.0f, PHASE_PRESSURE_MAX_BAR);
        addPhase(p, "Decline", PhaseType::PRESSURE, declineBar, (float)(o["pressure_decline_ms"] | 8000) / 1000.0f);
        used += p.phases[p.phaseCount - 1].durationS;
        last = declineBar;
      }
      addPhase(p, "Hold", PhaseType::PRESSURE, last, (float)p.maxSec - used);
    } else {
      addPhase(p, "Extract", PhaseType::POWER, (float)clampi(powers.brewPct, 10, 100), (float)p.maxSec - used);
    }

    const char *err = nullptr;
    if (!profileValidate(p, &err)) { (*warnings)++; continue; }
    out[n++] = p;
  }
  return n;
}
```

- [ ] **Step 7: Run the tests to confirm they pass**

Run: `& $pio test -e native -f test_profile_codec`
Expected: `18 Tests 0 Failures`, `PASSED`. If `test_make_old_name_truncates_to_twenty` fails, check that `keep` is 14 (20 − 6).

- [ ] **Step 8: Commit**

```powershell
git add include/config.h include/profile_types.h include/profile_codec.h src/profile_codec.cpp test/test_profile_codec
git commit -m "Add phase-profile codec, built-in profiles and legacy migration`n`nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Shot planner + curves

**Files:**
- Create: `include/shot_planner.h`, `src/shot_planner.cpp`
- Test: `test/test_shot_planner/test_main.cpp`

**Interfaces:**
- Consumes: `Profile`, `Phase`, `PhaseType`, `Curve` (Task 3); `PumpModelParams`, `PumpModelState`, `dutyForPressure`, `dutyForFlow`, `pumpFlowFull`, `pumpModelReset`, `pumpModelStep` (Task 1); `builtinProfilesLoad` (Task 3, used in tests).
- Produces (used by Task 7):
  ```cpp
  float curveEval(Curve c, float x); // x clamped to 0..1, returns 0..1
  struct PlannerOutput { bool running; bool done; float dutyPct; int phaseIndex; int phaseCount;
                         const char *phaseName; PhaseType phaseType; float phaseTarget;
                         float pressureEst; float mlEst; float elapsedS; };
  class ShotPlanner {
   public:
    void start(const Profile &profile, const PumpModelParams &model);
    PlannerOutput tick(float dtS, float measuredBar, bool sensorUsable);
    void stop();
    bool running() const;
  };
  ```
  When idle, `phaseIndex = -1` and `phaseName = ""`. On the tick that finishes the shot, `done = true` and `dutyPct = 0`.

- [ ] **Step 1: Write the failing tests**

Create `test/test_shot_planner/test_main.cpp`:
```cpp
#include <string.h>
#include <unity.h>

#include "profile_codec.h"
#include "shot_planner.h"

static PumpModelParams model(float trim = 0.0f) { return {0.213f, 50.0f, trim}; }

static Profile makeProfile(int maxSec) {
  Profile p;
  memset(&p, 0, sizeof p);
  strcpy(p.name, "T");
  p.temp = 93.0f;
  p.maxSec = (uint16_t)maxSec;
  p.pressureScale = 1.0f;
  return p;
}

static Phase &addPhase(Profile &p, PhaseType t, float target, float dur, Curve c = Curve::INSTANT, float ramp = 0) {
  Phase &f = p.phases[p.phaseCount++];
  memset(&f, 0, sizeof f);
  strcpy(f.name, "ph");
  f.type = t;
  f.target = target;
  f.durationS = dur;
  f.curve = c;
  f.rampS = ramp;
  f.stopPressGte = -1.0f;
  f.stopPressLte = -1.0f;
  return f;
}

// Runs the planner for `seconds` (or until done) and returns the last output.
static PlannerOutput runFor(ShotPlanner &pl, float seconds, float bar = 0.0f, bool usable = false) {
  PlannerOutput o = {};
  int n = (int)(seconds / 0.05f + 0.5f);
  for (int i = 0; i < n; i++) {
    o = pl.tick(0.05f, bar, usable);
    if (o.done) break;
  }
  return o;
}

void setUp() {}
void tearDown() {}

void test_curve_endpoints_and_monotonic() {
  Curve curves[] = {Curve::LINEAR, Curve::EASE_IN, Curve::EASE_OUT, Curve::EASE_IN_OUT};
  for (Curve c : curves) {
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, curveEval(c, 0.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, curveEval(c, 1.0f));
    float prev = -1.0f;
    for (float x = 0.0f; x <= 1.0f; x += 0.01f) {
      float y = curveEval(c, x);
      TEST_ASSERT_TRUE(y >= prev - 1e-6f);
      prev = y;
    }
  }
  TEST_ASSERT_EQUAL_FLOAT(1.0f, curveEval(Curve::INSTANT, 0.0f));
  TEST_ASSERT_EQUAL_FLOAT(1.0f, curveEval(Curve::LINEAR, 7.0f)); // clamped
}

void test_idle_planner_reports_nothing() {
  ShotPlanner pl;
  PlannerOutput o = pl.tick(0.05f, 0.0f, false);
  TEST_ASSERT_FALSE(o.running);
  TEST_ASSERT_EQUAL_INT(-1, o.phaseIndex);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, o.dutyPct);
}

void test_phases_advance_by_duration_power_and_soak() {
  Profile p = makeProfile(30);
  addPhase(p, PhaseType::POWER, 50.0f, 2.0f);
  addPhase(p, PhaseType::SOAK, 0.0f, 3.0f);
  ShotPlanner pl;
  pl.start(p, model());
  PlannerOutput o = runFor(pl, 1.0f);
  TEST_ASSERT_EQUAL_INT(0, o.phaseIndex);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, o.dutyPct);
  o = runFor(pl, 2.0f); // t = 3
  TEST_ASSERT_EQUAL_INT(1, o.phaseIndex);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, o.dutyPct);
  o = runFor(pl, 2.2f); // t = 5.2
  TEST_ASSERT_TRUE(o.done);
  TEST_ASSERT_FALSE(pl.running());
}

void test_max_sec_ends_shot_early() {
  Profile p = makeProfile(5);
  addPhase(p, PhaseType::POWER, 60.0f, 20.0f);
  ShotPlanner pl;
  pl.start(p, model());
  PlannerOutput o = runFor(pl, 6.0f);
  TEST_ASSERT_TRUE(o.done);
  TEST_ASSERT_FLOAT_WITHIN(0.06f, 5.0f, o.elapsedS);
}

void test_linear_ramp_starts_from_previous_same_type_target() {
  Profile p = makeProfile(30);
  addPhase(p, PhaseType::PRESSURE, 3.0f, 2.0f);
  addPhase(p, PhaseType::PRESSURE, 9.0f, 4.0f, Curve::LINEAR, 2.0f);
  ShotPlanner pl;
  pl.start(p, model());
  PlannerOutput o = runFor(pl, 3.0f); // 1 s into the 2 s ramp 3 -> 9
  TEST_ASSERT_EQUAL_INT(1, o.phaseIndex);
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 6.0f, o.phaseTarget); // +-1 tick of float time accumulation
}

void test_pressure_scale_applies() {
  Profile p = makeProfile(30);
  p.pressureScale = 0.8f;
  addPhase(p, PhaseType::PRESSURE, 9.0f, 10.0f);
  ShotPlanner pl;
  pl.start(p, model());
  PlannerOutput o = runFor(pl, 0.5f);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 7.2f, o.phaseTarget);
}

void test_trim_raises_settled_duty() {
  Profile p = makeProfile(30);
  addPhase(p, PhaseType::PRESSURE, 9.0f, 20.0f);
  ShotPlanner a, b;
  a.start(p, model(0.0f));
  b.start(p, model(20.0f));
  PlannerOutput oa = runFor(a, 10.0f);
  PlannerOutput ob = runFor(b, 10.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.3f, 9.0f, oa.pressureEst);
  TEST_ASSERT_TRUE(ob.dutyPct > oa.dutyPct + 5.0f);
}

void test_stop_condition_only_with_usable_sensor() {
  Profile p = makeProfile(30);
  Phase &f = addPhase(p, PhaseType::FLOW, 4.0f, 10.0f);
  f.stopPressGte = 4.0f;
  addPhase(p, PhaseType::PRESSURE, 9.0f, 10.0f);
  ShotPlanner pl;
  pl.start(p, model());
  PlannerOutput o = runFor(pl, 1.0f, 5.0f, false);
  TEST_ASSERT_EQUAL_INT(0, o.phaseIndex);
  o = runFor(pl, 0.1f, 5.0f, true);
  TEST_ASSERT_EQUAL_INT(1, o.phaseIndex);
}

void test_flow_phase_respects_bar_limit() {
  Profile p = makeProfile(30);
  Phase &f = addPhase(p, PhaseType::FLOW, 6.0f, 15.0f);
  f.limit = 4.0f;
  ShotPlanner pl;
  pl.start(p, model());
  PlannerOutput o = runFor(pl, 12.0f);
  TEST_ASSERT_TRUE(o.pressureEst <= 4.3f);
  TEST_ASSERT_TRUE(o.mlEst > 30.0f);
}

void test_every_builtin_runs_to_completion_with_sane_duty() {
  Profile builtins[PROFILE_BUILTIN_MAX];
  int n = builtinProfilesLoad(builtins, PROFILE_BUILTIN_MAX);
  TEST_ASSERT_EQUAL_INT(7, n);
  for (int i = 0; i < n; i++) {
    ShotPlanner pl;
    pl.start(builtins[i], model());
    PlannerOutput o = {};
    int ticks = 0;
    do {
      o = pl.tick(0.05f, 0.0f, false);
      TEST_ASSERT_TRUE_MESSAGE(o.dutyPct >= 0.0f && o.dutyPct <= 100.0f, builtins[i].name);
      ticks++;
    } while (!o.done && ticks < 200 * 20);
    TEST_ASSERT_TRUE_MESSAGE(o.done, builtins[i].name);
    TEST_ASSERT_TRUE_MESSAGE(o.elapsedS <= builtins[i].maxSec + 0.1f, builtins[i].name);
  }
}

static int runAll() {
  UNITY_BEGIN();
  RUN_TEST(test_curve_endpoints_and_monotonic);
  RUN_TEST(test_idle_planner_reports_nothing);
  RUN_TEST(test_phases_advance_by_duration_power_and_soak);
  RUN_TEST(test_max_sec_ends_shot_early);
  RUN_TEST(test_linear_ramp_starts_from_previous_same_type_target);
  RUN_TEST(test_pressure_scale_applies);
  RUN_TEST(test_trim_raises_settled_duty);
  RUN_TEST(test_stop_condition_only_with_usable_sensor);
  RUN_TEST(test_flow_phase_respects_bar_limit);
  RUN_TEST(test_every_builtin_runs_to_completion_with_sane_duty);
  return UNITY_END();
}

#ifdef ARDUINO
#include <Arduino.h>
void setup() { delay(2000); runAll(); }
void loop() {}
#else
int main() { return runAll(); }
#endif
```

- [ ] **Step 2: Run the tests to confirm they fail**

Run: `& $pio test -e native -f test_shot_planner`
Expected: a build failure, `shot_planner.h: No such file or directory`.

- [ ] **Step 3: Create `include/shot_planner.h`**

```cpp
#pragma once

#include "profile_types.h"
#include "pump_model.h"

// Phase sequencer (design spec 2026-10-02 §4.2). Pure C++: time comes in as
// dtS per tick, so tests drive it without millis(). The caller (main.cpp's
// controlTick, every 50 ms under stateMutex) sends dutyPct to the dimmer;
// the safety cutoffs that run after it still win.

// Ease curve value for x in 0..1 (clamped). INSTANT returns 1.
float curveEval(Curve c, float x);

struct PlannerOutput {
  bool running;
  bool done;            // true on the tick the shot finished (phases or max_sec)
  float dutyPct;        // 0..100
  int phaseIndex;       // -1 when idle
  int phaseCount;
  const char *phaseName; // points into the planner's own profile copy; "" when idle
  PhaseType phaseType;
  float phaseTarget;    // current interpolated target, in the phase's own unit
  float pressureEst;    // P_est (bar)
  float mlEst;          // estimated water pumped this shot (ml)
  float elapsedS;
};

class ShotPlanner {
 public:
  void start(const Profile &profile, const PumpModelParams &model);
  // measuredBar/sensorUsable: the real sensor reading, only trusted (for
  // duty laws and stop conditions) when sensorUsable is true.
  PlannerOutput tick(float dtS, float measuredBar, bool sensorUsable);
  void stop();
  bool running() const { return running_; }

 private:
  void enterPhase(float pressureNow);
  float targetAt(float t) const;
  float scaledStop(float v) const { return v * profile_.pressureScale; }

  Profile profile_ = {};
  PumpModelParams model_ = {};
  PumpModelState state_ = {};
  bool running_ = false;
  int phaseIdx_ = 0;
  float phaseElapsed_ = 0.0f;
  float elapsed_ = 0.0f;
  float phaseStart_ = 0.0f;
  float phaseEnd_ = 0.0f;
  bool hasPrev_ = false;
  PhaseType prevType_ = PhaseType::SOAK;
  float prevEnd_ = 0.0f;
  float lastDuty_ = 0.0f;
  float lastFlow_ = 0.0f;
};
```

- [ ] **Step 4: Create `src/shot_planner.cpp`**

```cpp
#include "shot_planner.h"

#include <math.h>

float curveEval(Curve c, float x) {
  if (!(x > 0.0f)) x = 0.0f;
  if (x > 1.0f) x = 1.0f;
  switch (c) {
    case Curve::LINEAR: return x;
    case Curve::EASE_IN: return x * x * x;
    case Curve::EASE_OUT: { float u = 1.0f - x; return 1.0f - u * u * u; }
    case Curve::EASE_IN_OUT: {
      if (x < 0.5f) return 4.0f * x * x * x;
      float u = -2.0f * x + 2.0f;
      return 1.0f - u * u * u / 2.0f;
    }
    default: return 1.0f; // INSTANT
  }
}

void ShotPlanner::start(const Profile &profile, const PumpModelParams &model) {
  profile_ = profile;
  model_ = model;
  pumpModelReset(state_);
  phaseIdx_ = 0;
  phaseElapsed_ = 0.0f;
  elapsed_ = 0.0f;
  hasPrev_ = false;
  lastDuty_ = 0.0f;
  lastFlow_ = 0.0f;
  running_ = profile_.phaseCount > 0;
  if (running_) enterPhase(0.0f);
}

void ShotPlanner::stop() { running_ = false; }

// Where the new phase's ramp starts (design spec §3.2): an explicit start;
// else the previous phase's reached value if it was the same type; else
// the live state in this phase's unit (pressure now, flow now, duty now).
void ShotPlanner::enterPhase(float pressureNow) {
  const Phase &ph = profile_.phases[phaseIdx_];
  float scale = profile_.pressureScale;
  phaseEnd_ = ph.type == PhaseType::PRESSURE ? ph.target * scale : ph.target;
  if (ph.hasStart) {
    phaseStart_ = ph.type == PhaseType::PRESSURE ? ph.start * scale : ph.start;
  } else if (hasPrev_ && prevType_ == ph.type && ph.type != PhaseType::SOAK) {
    phaseStart_ = prevEnd_;
  } else if (ph.type == PhaseType::PRESSURE) {
    phaseStart_ = pressureNow;
  } else if (ph.type == PhaseType::FLOW) {
    phaseStart_ = lastFlow_;
  } else if (ph.type == PhaseType::POWER) {
    phaseStart_ = lastDuty_ * 100.0f;
  } else {
    phaseStart_ = 0.0f;
  }
}

float ShotPlanner::targetAt(float t) const {
  const Phase &ph = profile_.phases[phaseIdx_];
  if (ph.curve == Curve::INSTANT || !(ph.rampS > 0.0f)) return phaseEnd_;
  return phaseStart_ + (phaseEnd_ - phaseStart_) * curveEval(ph.curve, t / ph.rampS);
}

PlannerOutput ShotPlanner::tick(float dtS, float measuredBar, bool sensorUsable) {
  PlannerOutput o = {};
  o.phaseIndex = -1;
  o.phaseName = "";
  if (!running_) return o;
  if (!(dtS > 0.0f)) dtS = 0.0f;
  elapsed_ += dtS;
  phaseElapsed_ += dtS;
  float pNow = sensorUsable ? measuredBar : state_.pressureBar;

  // Advance through every phase whose time (or stop condition) is up -
  // carrying leftover time so a long tick can't stretch the shot.
  while (phaseIdx_ < profile_.phaseCount) {
    const Phase &ph = profile_.phases[phaseIdx_];
    bool timeUp = phaseElapsed_ >= ph.durationS;
    bool stopHit = sensorUsable &&
                   ((ph.stopPressGte >= 0.0f && measuredBar >= scaledStop(ph.stopPressGte)) ||
                    (ph.stopPressLte >= 0.0f && measuredBar <= scaledStop(ph.stopPressLte)));
    if (!timeUp && !stopHit) break;
    float carry = timeUp ? phaseElapsed_ - ph.durationS : 0.0f;
    prevEnd_ = targetAt(timeUp ? ph.durationS : phaseElapsed_);
    prevType_ = ph.type;
    hasPrev_ = true;
    phaseIdx_++;
    phaseElapsed_ = carry;
    if (phaseIdx_ < profile_.phaseCount) enterPhase(pNow);
  }

  if (phaseIdx_ >= profile_.phaseCount || elapsed_ >= (float)profile_.maxSec) {
    running_ = false;
    o.done = true;
    o.phaseCount = profile_.phaseCount;
    o.pressureEst = state_.pressureBar;
    o.mlEst = state_.mlPumped;
    o.elapsedS = elapsed_;
    return o;
  }

  const Phase &ph = profile_.phases[phaseIdx_];
  float target = targetAt(phaseElapsed_);
  float duty = 0.0f;
  switch (ph.type) {
    case PhaseType::PRESSURE:
      duty = dutyForPressure(model_, pNow, target);
      if (ph.limit > 0.0f) duty = fminf(duty, dutyForFlow(model_, pNow, ph.limit));
      break;
    case PhaseType::FLOW: {
      float limitBar = ph.limit > 0.0f ? ph.limit * profile_.pressureScale : 0.0f;
      float pForFlow = (limitBar > 0.0f && pNow > limitBar) ? limitBar : pNow;
      duty = dutyForFlow(model_, pForFlow, target);
      if (limitBar > 0.0f) duty = fminf(duty, dutyForPressure(model_, pNow, limitBar));
      break;
    }
    case PhaseType::POWER: duty = target / 100.0f; break;
    default: duty = 0.0f; break; // SOAK
  }
  if (!(duty >= 0.0f)) duty = 0.0f;
  if (duty > 1.0f) duty = 1.0f;

  pumpModelStep(model_, state_, duty, dtS);
  lastDuty_ = duty;
  lastFlow_ = duty * pumpFlowFull(model_, pNow);

  o.running = true;
  o.dutyPct = duty * 100.0f;
  o.phaseIndex = phaseIdx_;
  o.phaseCount = profile_.phaseCount;
  o.phaseName = ph.name;
  o.phaseType = ph.type;
  o.phaseTarget = target;
  o.pressureEst = state_.pressureBar;
  o.mlEst = state_.mlPumped;
  o.elapsedS = elapsed_;
  return o;
}
```

- [ ] **Step 5: Run all native tests**

Run: `& $pio test -e native`
Expected: all 4 suites `PASSED` (13 + 7 + 18 + 10 tests).

- [ ] **Step 6: Commit**

```powershell
git add include/shot_planner.h src/shot_planner.cpp test/test_shot_planner
git commit -m "Add phase shot planner with ease curves and pump-model duty laws`n`nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Dimmer hardening

**Files:**
- Modify: `include/config.h` (the dimmer block around `DIMMER_AC_HALF_CYCLE_US`)
- Modify (rewrite): `include/dimmer.h`, `src/dimmer.cpp`
- Modify: `src/main.cpp` (call `dimmerTick()` in `controlTick()`)

**Interfaces:**
- Consumes: `PIN_DIMMER_GATE`, `PIN_DIMMER_ZC`, `DIMMER_AC_HALF_CYCLE_US`.
- Produces (used by Tasks 6, 7 and 9):
  ```cpp
  enum class MainsState { OK, ABSENT, UNSTABLE };
  void dimmerInit();
  void dimmerSetPowerPercent(float percent);          // rises slewed, falls instantly
  void dimmerSetPowerPercentImmediate(float percent); // no slew - calibration only
  void dimmerTick(uint32_t dtMs);                     // every control tick
  float dimmerGetPowerPercent();                      // the APPLIED (firing) percent
  uint32_t dimmerGetZcCount();
  uint32_t dimmerGetFiredCycles();
  MainsState dimmerGetMainsState();
  float dimmerGetMainsHz();
  const char *mainsStateName(MainsState s);           // "ok" / "absent" / "unstable"
  ```

- [ ] **Step 1: Update the dimmer constants**

In `config.h`, replace these two lines and the comment sentence that describes them (`DIMMER_MIN_FIRING_DELAY_US is how close...` through `...TRIAC modules need.`):
```cpp
#define DIMMER_MIN_FIRING_DELAY_US 300
#define DIMMER_GATE_PULSE_US 100
```
with:
```cpp
// Gate drive (2026-10-02 hardening, design spec §5): the gate goes HIGH in
// the zero-cross ISR itself and is held DIMMER_GATE_HOLD_US, dropped by a
// gptimer alarm. Long enough to span the true crossing whatever the
// module's ZC offset, and for the pump's inductive current to rise past the
// TRIAC's latching current (a 100 us pulse could end first = lost stroke).
#define DIMMER_GATE_HOLD_US 1500
// Duty rises at most this fast (pump hammer on shot start/phase steps);
// decreases - including any cut to 0 - are always instant.
#define DIMMER_SLEW_PCT_PER_S 250
// Mains health: no accepted crossing for this long = ABSENT (Brew switch
// off); a measured frequency outside MIN..MAX = UNSTABLE.
#define DIMMER_MAINS_ABSENT_MS 100
#define DIMMER_MAINS_HZ_MIN 45.0f
#define DIMMER_MAINS_HZ_MAX 65.0f
#define DIMMER_MAINS_HZ_DEFAULT 50.0f
```
Keep `DIMMER_AC_HALF_CYCLE_US`. It still seeds the initial debounce window.

- [ ] **Step 2: Rewrite `include/dimmer.h`**

```cpp
#pragma once

#include <stdint.h>

// AC dimmer driver (HARDWARE_ROADMAP.md item 8). Pulse-skip modulation:
// whole mains cycles are fired or skipped (Bresenham-spread), which is what
// a diode-fed ULKA pump responds to - one stroke per fired cycle. Hardened
// 2026-10-02 (design spec §5): long gate hold via a gptimer, mains
// frequency auto-detect + zero-cross watchdog, stroke counters, slew limit.

enum class MainsState { OK, ABSENT, UNSTABLE };

void dimmerInit();

// Target power 0-100 (clamped). Rises toward it at DIMMER_SLEW_PCT_PER_S
// (advanced by dimmerTick()); any decrease, including 0, applies instantly.
void dimmerSetPowerPercent(float percent);
// Same, but with no slew - used only by the pump calibration run.
void dimmerSetPowerPercentImmediate(float percent);
// Call every control tick: advances the slew and updates mains health.
void dimmerTick(uint32_t dtMs);

// The percent actually being fired right now (after slew).
float dimmerGetPowerPercent();

uint32_t dimmerGetZcCount();     // accepted zero-crossings since boot
uint32_t dimmerGetFiredCycles(); // fired mains cycles (= pump strokes) since boot
MainsState dimmerGetMainsState();
float dimmerGetMainsHz();        // last valid measurement; DIMMER_MAINS_HZ_DEFAULT until then
const char *mainsStateName(MainsState s);
```

- [ ] **Step 3: Rewrite `src/dimmer.cpp`**

```cpp
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
```

- [ ] **Step 4: Call `dimmerTick()` every control tick**

In `src/main.cpp` `controlTick()`, immediately after `lockState();` at the top of the function, insert:
```cpp
  // Dimmer slew + mains health (dimmer.cpp) - every tick, before anything
  // below sets a new duty, so the slew always has a fresh dt.
  static unsigned long lastDimmerTickMs = 0;
  if (lastDimmerTickMs == 0) lastDimmerTickMs = now;
  dimmerTick((uint32_t)(now - lastDimmerTickMs));
  lastDimmerTickMs = now;
```

- [ ] **Step 5: Build**

Run: `& $pio run -e esp32-s3-devkitc-1`
Expected: `SUCCESS`. If `driver/gptimer.h` isn't found, the pioarduino core is IDF 5.x and the header is `#include "driver/gptimer.h"`. Check spelling first; don't swap in a different timer API.

- [ ] **Step 6: Re-run the native tests to catch regressions**

Run: `& $pio test -e native`
Expected: all `PASSED`.

- [ ] **Step 7: Commit**

```powershell
git add include/config.h include/dimmer.h src/dimmer.cpp src/main.cpp
git commit -m "Harden dimmer: long gptimer gate hold, mains detect/watchdog, stroke counter, slew`n`nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Pump calibration runner + API

**Files:**
- Create: `include/pump_calibration.h`, `src/pump_calibration.cpp`
- Modify: `src/main.cpp` (init, tick, abort on shot start)
- Modify: `src/web.cpp` (`/status` fields, `/update` actions, 409 errors, settings export)

**Interfaces:**
- Consumes: `calRunMlPerClick`, `calPlausible`, `calEvaluate` (Task 2); `dimmerSetPowerPercentImmediate`, `dimmerSetPowerPercent`, `dimmerGetPowerPercent`, `dimmerGetFiredCycles`, `dimmerGetMainsState` (Task 5); the globals `shotInProgress`, `autotuneState` and `currentPressure` from `main.cpp`.
- Produces (used by Tasks 7 and 10):
  ```cpp
  enum class PumpCalState { IDLE, RUNNING, AWAIT_GRAMS, ABORTED };
  extern float pumpMlPerClick; extern bool pumpCalibrated;
  void pumpCalInit(); void pumpCalTick(unsigned long nowMs);
  bool pumpCalStartRun(int runIndex, const char **err); // -1 = next empty slot
  void pumpCalStop(); void pumpCalAbort(const char *reason);
  bool pumpCalRecordGrams(float grams, const char **err);
  bool pumpCalSave(const char **err); void pumpCalClear();
  bool pumpCalSetValue(float mlPerClick);
  PumpCalState pumpCalGetState(); const char *pumpCalStateName(); const char *pumpCalAbortReason();
  int pumpCalActiveRun(); unsigned long pumpCalRunElapsedMs(unsigned long nowMs); float pumpCalRunValue(int i);
  ```
  - **`/status` fields:** `pump_calibrated`, `pump_ml_per_click`, `pump_cal_state`, `pump_cal_reason`, `pump_cal_run`, `pump_cal_elapsed_ms`, `pump_cal_runs` (3 numbers or `null`), `pump_mains`, `mains_hz`, `zc_count`, `fired_cycles`.
  - **`/update` params:** `pump_cal=run|stop|save|cancel`, `pump_cal_redo=1..3`, `pump_cal_grams=<g>`, `pump_mlpc=<v>`. An error responds **409** with plain-text `actionErr`.

- [ ] **Step 1: Create `include/pump_calibration.h`**

```cpp
#pragma once

#include <Arduino.h>

// Pump calibration runner (design spec 2026-10-02 §6): 3 free-flow runs at
// 100% for PUMP_CAL_RUN_MS, the user weighs each cup, the mean ml/stroke
// becomes the pump model's scale. Owns pumpMlPerClick (NVS "pump_mlpc").
// All functions are called with stateMutex held (controlTick / /update).

enum class PumpCalState { IDLE, RUNNING, AWAIT_GRAMS, ABORTED };

extern float pumpMlPerClick; // the model's ml/stroke (default until calibrated)
extern bool pumpCalibrated;  // true once a calibration has been saved

void pumpCalInit();
void pumpCalTick(unsigned long nowMs);
bool pumpCalStartRun(int runIndex, const char **err); // 0..2, or -1 = next empty slot
void pumpCalStop();                                   // Stop button
void pumpCalAbort(const char *reason);                // discards the running run
bool pumpCalRecordGrams(float grams, const char **err);
bool pumpCalSave(const char **err);
void pumpCalClear();                                  // discard unsaved runs
bool pumpCalSetValue(float mlPerClick);               // settings import
PumpCalState pumpCalGetState();
const char *pumpCalStateName();
const char *pumpCalAbortReason();
int pumpCalActiveRun();
unsigned long pumpCalRunElapsedMs(unsigned long nowMs);
float pumpCalRunValue(int i); // < 0 = not measured yet
```

- [ ] **Step 2: Create `src/pump_calibration.cpp`**

```cpp
#include "pump_calibration.h"

#include <Preferences.h>
#include <time.h>

#include "config.h"
#include "dimmer.h"
#include "pump_cal_math.h"

extern bool shotInProgress;
extern AutotuneState autotuneState;
extern float currentPressure;

float pumpMlPerClick = PUMP_ML_PER_CLICK_DEFAULT;
bool pumpCalibrated = false;

static PumpCalState state = PumpCalState::IDLE;
static float runs[PUMP_CAL_RUNS] = {-1.0f, -1.0f, -1.0f};
static int activeRun = -1;
static unsigned long runStartMs = 0;
static uint32_t runStartFired = 0;
static uint32_t runCycles = 0;
static const char *abortReason = "";

void pumpCalInit() {
  Preferences prefs;
  prefs.begin("gaggia", true);
  float v = prefs.getFloat("pump_mlpc", -1.0f);
  prefs.end();
  if (calPlausible(v)) {
    pumpMlPerClick = v;
    pumpCalibrated = true;
  }
}

bool pumpCalStartRun(int runIndex, const char **err) {
  if (state == PumpCalState::RUNNING) { *err = "A calibration run is already running"; return false; }
  if (state == PumpCalState::AWAIT_GRAMS) { *err = "Enter the grams for the last run first"; return false; }
  if (shotInProgress) { *err = "Stop the shot first"; return false; }
  if (autotuneState == AutotuneState::RUNNING) { *err = "Autotune is running"; return false; }
  if (dimmerGetMainsState() != MainsState::OK) { *err = "No mains at the pump - turn the Brew switch on"; return false; }
  if (runIndex < 0) {
    for (int i = 0; i < PUMP_CAL_RUNS; i++) {
      if (runs[i] < 0.0f) { runIndex = i; break; }
    }
    if (runIndex < 0) { *err = "All 3 runs are done - Save, or Redo one"; return false; }
  }
  if (runIndex >= PUMP_CAL_RUNS) { *err = "Bad run number"; return false; }
  activeRun = runIndex;
  runs[runIndex] = -1.0f;
  runStartMs = millis();
  runStartFired = dimmerGetFiredCycles();
  abortReason = "";
  state = PumpCalState::RUNNING;
  dimmerSetPowerPercentImmediate(100.0f); // full power from the first cycle, no slew
  Serial.printf("pump cal: run %d started\n", runIndex + 1);
  return true;
}

void pumpCalAbort(const char *reason) {
  if (state != PumpCalState::RUNNING && state != PumpCalState::AWAIT_GRAMS) return;
  dimmerSetPowerPercent(0.0f);
  if (activeRun >= 0) runs[activeRun] = -1.0f; // never keep a short measurement
  activeRun = -1;
  abortReason = reason;
  state = PumpCalState::ABORTED;
  Serial.printf("pump cal: run aborted (%s)\n", reason);
}

void pumpCalStop() {
  if (state == PumpCalState::RUNNING) pumpCalAbort("stopped");
}

// Runs AFTER controlTick()'s pump safety cutoffs, so a cut they made this
// tick is visible here as the dimmer no longer being at 100%.
void pumpCalTick(unsigned long nowMs) {
  if (state != PumpCalState::RUNNING) return;
  if (dimmerGetMainsState() == MainsState::ABSENT) { pumpCalAbort("mains lost - Brew switch off?"); return; }
  if (dimmerGetPowerPercent() < 99.0f) { pumpCalAbort("pump cut by a safety check"); return; }
  if (nowMs - runStartMs >= PUMP_CAL_RUN_MS) {
    runCycles = dimmerGetFiredCycles() - runStartFired;
    dimmerSetPowerPercent(0.0f);
    state = PumpCalState::AWAIT_GRAMS;
    Serial.printf("pump cal: run %d done, %lu strokes\n", activeRun + 1, (unsigned long)runCycles);
  }
}

bool pumpCalRecordGrams(float grams, const char **err) {
  if (state != PumpCalState::AWAIT_GRAMS) { *err = "No run is waiting for grams"; return false; }
  float v = calRunMlPerClick(grams, runCycles);
  if (!calPlausible(v)) { *err = "That works out outside 0.15-0.40 ml per stroke - check the grams"; return false; }
  runs[activeRun] = v;
  activeRun = -1;
  state = PumpCalState::IDLE;
  return true;
}

bool pumpCalSave(const char **err) {
  if (state == PumpCalState::RUNNING || state == PumpCalState::AWAIT_GRAMS) { *err = "Finish the current run first"; return false; }
  for (int i = 0; i < PUMP_CAL_RUNS; i++) {
    if (runs[i] < 0.0f) { *err = "Do all 3 runs first"; return false; }
  }
  CalEvaluation e = calEvaluate(runs);
  if (e.outlier >= 0) {
    static char msg[64];
    snprintf(msg, sizeof msg, "Run %d is more than 10%% off the others - redo it", e.outlier + 1);
    *err = msg;
    return false;
  }
  pumpMlPerClick = e.mean;
  pumpCalibrated = true;
  Preferences prefs;
  prefs.begin("gaggia", false);
  prefs.putFloat("pump_mlpc", pumpMlPerClick);
  prefs.putFloat("pump_cal_r0", runs[0]);
  prefs.putFloat("pump_cal_r1", runs[1]);
  prefs.putFloat("pump_cal_r2", runs[2]);
  prefs.putULong("pump_cal_at", (unsigned long)time(nullptr));
  prefs.end();
  Serial.printf("pump cal: saved %.4f ml/stroke\n", pumpMlPerClick);
  pumpCalClear();
  return true;
}

void pumpCalClear() {
  if (state == PumpCalState::RUNNING) pumpCalAbort("cleared");
  for (int i = 0; i < PUMP_CAL_RUNS; i++) runs[i] = -1.0f;
  activeRun = -1;
  abortReason = "";
  state = PumpCalState::IDLE;
}

bool pumpCalSetValue(float mlPerClick) {
  if (!calPlausible(mlPerClick)) return false;
  pumpMlPerClick = mlPerClick;
  pumpCalibrated = true;
  Preferences prefs;
  prefs.begin("gaggia", false);
  prefs.putFloat("pump_mlpc", pumpMlPerClick);
  prefs.end();
  return true;
}

PumpCalState pumpCalGetState() { return state; }

const char *pumpCalStateName() {
  switch (state) {
    case PumpCalState::RUNNING: return "running";
    case PumpCalState::AWAIT_GRAMS: return "await_grams";
    case PumpCalState::ABORTED: return "aborted";
    default: return "idle";
  }
}

const char *pumpCalAbortReason() { return abortReason; }
int pumpCalActiveRun() { return activeRun; }
unsigned long pumpCalRunElapsedMs(unsigned long nowMs) {
  return state == PumpCalState::RUNNING ? nowMs - runStartMs : 0;
}
float pumpCalRunValue(int i) { return (i >= 0 && i < PUMP_CAL_RUNS) ? runs[i] : -1.0f; }
```

- [ ] **Step 3: Hook it into `main.cpp`**

1. Add `#include "pump_calibration.h"` after `#include "profiles.h"`.
2. In `setup()`, right after `profilesInit();`, add `pumpCalInit();`.
3. In `startShot()`, as the first line after `if (shotInProgress) return;`, add:
   ```cpp
   if (pumpCalGetState() == PumpCalState::RUNNING) pumpCalAbort("shot started");
   ```
4. In `controlTick()`, immediately **after** the closing `}` of the pressure-safety block (the one starting `if ((pressureClosedLoopActive && pressureFault) || currentPressure > PUMP_MAX_SAFETY_BAR) {`), add:
   ```cpp
   // Pump calibration run (pump_calibration.cpp) - after the safety cutoffs
   // so a cut they just made aborts the run instead of being measured.
   pumpCalTick(now);
   ```

- [ ] **Step 4: Expose it over the web API (`web.cpp`)**

1. Add `#include "pump_calibration.h"` after `#include "profiles.h"`.
2. In `handleStatus()`, before `unlockState();` in the snapshot block, add:
   ```cpp
   bool snapPumpCalibrated = pumpCalibrated;
   float snapPumpMlpc = pumpMlPerClick;
   String snapCalState = pumpCalStateName();
   String snapCalReason = pumpCalAbortReason();
   int snapCalRun = pumpCalActiveRun();
   unsigned long snapCalElapsed = pumpCalRunElapsedMs(millis());
   float snapCalRuns[3] = {pumpCalRunValue(0), pumpCalRunValue(1), pumpCalRunValue(2)};
   String snapMains = mainsStateName(dimmerGetMainsState());
   float snapMainsHz = dimmerGetMainsHz();
   uint32_t snapFired = dimmerGetFiredCycles();
   ```
3. Right after the line `json += ",\"dimmer_zc_count\":" + String(snapDimmerZcCount);`, add:
   ```cpp
   json += ",\"zc_count\":" + String(snapDimmerZcCount);
   json += ",\"fired_cycles\":" + String(snapFired);
   json += ",\"pump_mains\":\"" + snapMains + "\"";
   json += ",\"mains_hz\":" + String(snapMainsHz, 1);
   json += ",\"pump_calibrated\":" + String(snapPumpCalibrated ? "true" : "false");
   json += ",\"pump_ml_per_click\":" + String(snapPumpMlpc, 4);
   json += ",\"pump_cal_state\":\"" + snapCalState + "\"";
   json += ",\"pump_cal_reason\":\"" + snapCalReason + "\"";
   json += ",\"pump_cal_run\":" + String(snapCalRun);
   json += ",\"pump_cal_elapsed_ms\":" + String(snapCalElapsed);
   json += ",\"pump_cal_runs\":[";
   for (int i = 0; i < 3; i++) {
     if (i > 0) json += ",";
     json += snapCalRuns[i] < 0.0f ? String("null") : String(snapCalRuns[i], 4);
   }
   json += "]";
   ```
4. In `handleUpdate()`, right after `lockState();` (the one inside `handleUpdate`), declare:
   ```cpp
   // Actions that can be refused (calibration, profiles) set this; the
   // response is then 409 + the reason instead of the usual redirect, so
   // the Web UI can show WHY (xhr callback gets status + text).
   const char *actionErr = nullptr;
   ```
   Then, right after the shot-timer block (`if (hasArg("shot")) { ... }`), add:
   ```cpp
   // Pump calibration (pump_calibration.cpp, design spec §6)
   if (hasArg("pump_cal")) {
     String a = arg("pump_cal");
     if (a == "run") pumpCalStartRun(-1, &actionErr);
     else if (a == "stop") pumpCalStop();
     else if (a == "save") pumpCalSave(&actionErr);
     else if (a == "cancel") pumpCalClear();
   }
   if (hasArg("pump_cal_redo")) {
     int n = arg("pump_cal_redo").toInt();
     if (n < 1 || n > 3) actionErr = "Bad run number";
     else pumpCalStartRun(n - 1, &actionErr);
   }
   if (hasArg("pump_cal_grams")) pumpCalRecordGrams(arg("pump_cal_grams").toFloat(), &actionErr);
   if (hasArg("pump_mlpc") && !pumpCalSetValue(arg("pump_mlpc").toFloat())) actionErr = "pump_mlpc out of range (0.15-0.40)";
   ```
   Finally, replace the single line `request->redirect("/");` near the end of `handleUpdate()` with:
   ```cpp
   if (actionErr) {
     request->send(409, "text/plain", actionErr);
   } else {
     request->redirect("/");
   }
   ```
5. In the `/settings_export` handler, after the `q += "&descale_day_threshold=" ...` line, add:
   ```cpp
   if (pumpCalibrated) q += "&pump_mlpc=" + String(pumpMlPerClick, 4);
   ```

- [ ] **Step 5: Build**

Run: `& $pio run -e esp32-s3-devkitc-1`
Expected: `SUCCESS`.

- [ ] **Step 6: Bench check (board on USB, mains off)**

Flash: `& $pio run -e esp32-s3-devkitc-1 -t upload`. Then, from PowerShell:
```powershell
curl.exe -s http://gaggia.local/status | Select-String -Pattern '"pump_mains":"absent"','"pump_calibrated":false','"pump_cal_state":"idle"'
curl.exe -s -o NUL -w "%{http_code}\n" "http://gaggia.local/update?pump_cal=run"
curl.exe -s "http://gaggia.local/update?pump_cal=run"
```
Expected:
- all three patterns match;
- the second command prints `409`;
- the third prints `No mains at the pump - turn the Brew switch on`.

- [ ] **Step 7: Commit**

```powershell
git add include/pump_calibration.h src/pump_calibration.cpp src/main.cpp src/web.cpp
git commit -m "Add 3-run pump calibration runner and its web API`n`nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Switch firmware to phase profiles (store + planner + API)

This is one task because the three parts can't build separately. The old profile API (`profileGet`/`profileSave`/`applyProfile`) disappears in the same commit that its callers move off it.

**Files:**
- Rewrite: `include/profiles.h`, `src/profiles.cpp`
- Modify: `src/main.cpp`, `src/web.cpp`, `include/config.h`

**Interfaces:**
- Consumes: everything from Tasks 1–6.
- Produces (used by Tasks 8–10, as the web API):
  - **`profiles.h`:**
    ```cpp
    void profilesInit(int legacyBrewPct, int legacyPreinfusionPct);
    int profileCount(); const Profile *profileAt(int i); const Profile *profileFind(const char *name);
    String profilesListJson(); String profileLegacyNameAt(int legacyIndex);
    bool profileTweak(const char *name, const ProfileTweak &t, const char **err);
    bool profileDuplicate(const char *src, const char *newName, const char **err);
    bool profileRename(const char *oldName, const char *newName, const char **err);
    bool profileDeleteUser(const char *name, const char **err);
    bool profileReset(const char *name, const char **err);
    ```
  - **`main.cpp` globals:** `String activeProfileName`, `bool selectProfile(const char *name)`, `PlannerOutput plannerOut`, `float estPressureHistory[]`, `float targetPressureHistory[]`, `int estHistoryHead`, `int estHistoryCount`.
  - **`/status` adds:** `active_profile_name`, `active_max_sec`, `phase_name`, `phase_index`, `phase_count`, `phase_type`, `phase_target`, `pressure_est`, `ml_est`, `est_history`, `target_history`.
  - **`/status` removes:** `shot_auto_stop_sec`, `pump_brew_pct`, `pump_pi_pct`, `pi_fill_bar`, `pi_fill_max_s`, `pi_soak_s`, `active_profile`, `pi_enabled`, `pi_pulses`, `pi_on_ms`, `pi_off_ms`, `press_enabled`, `press_ramp_bar`, `press_ramp_ms`, `press_decline_enabled`, `press_decline_bar`, `press_decline_ms`.
  - **`/update`:** `profile_select=<name>`; `profile_tweak=<name>` with `temp`, `max_sec`, `pressure_scale`, `trim_pct`; `profile_duplicate=<name>&new_name=`; `profile_rename=<name>&new_name=`; `profile_delete=<name>`; `profile_reset=<name>`. Errors return 409.
  - **`/profiles`** returns `[{name, builtin, utility, temp, max_sec, pressure_scale, trim_pct, tweaked, phases:[{name, type, target, limit?, start?, curve?, ramp_s?, duration_s, stop?}]}]`.

- [ ] **Step 1: Rewrite `include/profiles.h`**

```cpp
#pragma once

#include <Arduino.h>

#include "profile_types.h"

// Shot profile store on LittleFS (design spec 2026-10-02 §3.1): built-ins
// from firmware (profile_codec.cpp) + up to PROFILE_MAX_COUNT user profiles,
// in PROFILE_STORE_PATH as {"version":1,"user":[...],"builtin_overrides":
// {name:{temp,max_sec,pressure_scale,trim_pct}}}. Everything is addressed by
// NAME (case-insensitive). Call every function with stateMutex held;
// returned pointers are valid until the next mutation.

// Mounts LittleFS, loads built-ins and the store. If the store doesn't
// exist but the legacy PROFILE_LOG_PATH does, backs it up byte-for-byte to
// PROFILE_LEGACY_BACKUP_PATH and migrates it (legacy*Pct = the old global
// pump powers, used for migrated pulse/extract phases).
void profilesInit(int legacyBrewPct, int legacyPreinfusionPct);

int profileCount();                         // built-ins + user
const Profile *profileAt(int i);            // tweaks applied
const Profile *profileFind(const char *name);
String profilesListJson();                  // GET /profiles
String profileLegacyNameAt(int legacyIndex); // old NVS "active_profile" index -> migrated name, "" if none

bool profileTweak(const char *name, const ProfileTweak &t, const char **err);
bool profileDuplicate(const char *src, const char *newName, const char **err);
bool profileRename(const char *oldName, const char *newName, const char **err);
bool profileDeleteUser(const char *name, const char **err);
bool profileReset(const char *name, const char **err);
```

- [ ] **Step 2: Rewrite `src/profiles.cpp`**

```cpp
#include "profiles.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include "config.h"
#include "profile_codec.h"

static Profile baseBuiltins[PROFILE_BUILTIN_MAX];
static bool builtinTweaked[PROFILE_BUILTIN_MAX];
static ProfileTweak builtinTweaks[PROFILE_BUILTIN_MAX];
static int builtinCount = 0;
static Profile users[PROFILE_MAX_COUNT];
static int userCount = 0;
static Profile effective[PROFILE_BUILTIN_MAX + PROFILE_MAX_COUNT];
static int effectiveCount = 0;
static String legacyNames[PROFILE_MAX_COUNT];
static int legacyCount = 0;

static int findBuiltin(const char *name) {
  for (int i = 0; i < builtinCount; i++)
    if (profileNameEquals(baseBuiltins[i].name, name)) return i;
  return -1;
}

static int findUser(const char *name) {
  for (int i = 0; i < userCount; i++)
    if (profileNameEquals(users[i].name, name)) return i;
  return -1;
}

static void rebuildEffective() {
  effectiveCount = 0;
  for (int i = 0; i < builtinCount; i++) {
    effective[effectiveCount] = baseBuiltins[i];
    if (builtinTweaked[i]) profileApplyTweak(effective[effectiveCount], builtinTweaks[i]);
    effectiveCount++;
  }
  for (int i = 0; i < userCount; i++) effective[effectiveCount++] = users[i];
}

// Write-then-rename so a power cut mid-save can't leave a half-written store.
static bool saveStore() {
  JsonDocument doc;
  doc["version"] = 1;
  JsonArray u = doc["user"].to<JsonArray>();
  for (int i = 0; i < userCount; i++) profileToJson(users[i], u.add<JsonObject>());
  JsonObject ov = doc["builtin_overrides"].to<JsonObject>();
  for (int i = 0; i < builtinCount; i++) {
    if (!builtinTweaked[i]) continue;
    JsonObject t = ov[baseBuiltins[i].name].to<JsonObject>();
    t["temp"] = builtinTweaks[i].temp;
    t["max_sec"] = builtinTweaks[i].maxSec;
    t["pressure_scale"] = builtinTweaks[i].pressureScale;
    t["trim_pct"] = builtinTweaks[i].trimPct;
  }
  const char *tmp = PROFILE_STORE_PATH ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) {
    Serial.println("profiles: can't open temp file for write");
    return false;
  }
  serializeJson(doc, f);
  f.close();
  LittleFS.remove(PROFILE_STORE_PATH);
  if (!LittleFS.rename(tmp, PROFILE_STORE_PATH)) {
    Serial.println("profiles: rename of temp store failed");
    return false;
  }
  return true;
}

static bool loadStore() {
  File f = LittleFS.open(PROFILE_STORE_PATH, "r");
  if (!f) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("profiles: %s unreadable (%s) - moved aside, starting from built-ins\n", PROFILE_STORE_PATH, err.c_str());
    LittleFS.remove(PROFILE_STORE_PATH ".bad");
    LittleFS.rename(PROFILE_STORE_PATH, PROFILE_STORE_PATH ".bad");
    return false;
  }
  for (JsonVariantConst v : doc["user"].as<JsonArrayConst>()) {
    if (userCount >= PROFILE_MAX_COUNT) break;
    const char *e = nullptr;
    Profile p;
    if (!profileFromJson(v.as<JsonObjectConst>(), p, &e)) {
      Serial.printf("profiles: skipped invalid user profile (%s)\n", e);
      continue;
    }
    p.builtin = false;
    users[userCount++] = p;
  }
  for (JsonPairConst kv : doc["builtin_overrides"].as<JsonObjectConst>()) {
    int bi = findBuiltin(kv.key().c_str());
    if (bi < 0) continue;
    JsonObjectConst t = kv.value().as<JsonObjectConst>();
    ProfileTweak tw = profileGetTweak(baseBuiltins[bi]);
    tw.temp = t["temp"] | tw.temp;
    tw.maxSec = t["max_sec"] | tw.maxSec;
    tw.pressureScale = t["pressure_scale"] | tw.pressureScale;
    tw.trimPct = t["trim_pct"] | tw.trimPct;
    builtinTweaks[bi] = tw;
    builtinTweaked[bi] = true;
  }
  return true;
}

static void migrateLegacy(int legacyBrewPct, int legacyPreinfusionPct) {
  if (!LittleFS.exists(PROFILE_LOG_PATH)) return;
  File f = LittleFS.open(PROFILE_LOG_PATH, "r");
  if (!f) return;
  String legacy = f.readString();
  f.close();

  // Backup FIRST, byte for byte - the 2026-08-23 migration lost data with no backup.
  File b = LittleFS.open(PROFILE_LEGACY_BACKUP_PATH, "w");
  if (!b || b.print(legacy) != legacy.length()) {
    if (b) b.close();
    Serial.println("profiles: legacy backup failed - NOT migrating, legacy file left untouched");
    return;
  }
  b.close();

  JsonDocument names;
  if (!deserializeJson(names, legacy)) {
    for (JsonVariantConst v : names.as<JsonArrayConst>()) {
      if (legacyCount >= PROFILE_MAX_COUNT) break;
      legacyNames[legacyCount++] = v["name"] | "";
    }
  }

  int warnings = 0;
  LegacyPowers powers = {legacyBrewPct, legacyPreinfusionPct};
  userCount = migrateLegacyProfiles(legacy.c_str(), powers, baseBuiltins, builtinCount, users, PROFILE_MAX_COUNT, &warnings);
  Serial.printf("profiles: migrated %d legacy profiles (%d warnings); backup at %s\n", userCount, warnings,
                PROFILE_LEGACY_BACKUP_PATH);
}

void profilesInit(int legacyBrewPct, int legacyPreinfusionPct) {
  builtinCount = builtinProfilesLoad(baseBuiltins, PROFILE_BUILTIN_MAX);
  if (builtinCount == 0) Serial.println("profiles: BUILT-IN TABLE FAILED TO PARSE");
  if (!LittleFS.begin(true)) {
    Serial.println("profiles: LittleFS mount failed - built-ins only");
    rebuildEffective();
    return;
  }
  if (LittleFS.exists(PROFILE_STORE_PATH)) {
    if (!loadStore()) saveStore();
  } else {
    migrateLegacy(legacyBrewPct, legacyPreinfusionPct);
    saveStore();
  }
  rebuildEffective();
}

int profileCount() { return effectiveCount; }
const Profile *profileAt(int i) { return (i >= 0 && i < effectiveCount) ? &effective[i] : nullptr; }

const Profile *profileFind(const char *name) {
  if (!name) return nullptr;
  for (int i = 0; i < effectiveCount; i++)
    if (profileNameEquals(effective[i].name, name)) return &effective[i];
  return nullptr;
}

String profilesListJson() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < effectiveCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    profileToJson(effective[i], o);
    int bi = effective[i].builtin ? findBuiltin(effective[i].name) : -1;
    o["tweaked"] = bi >= 0 && builtinTweaked[bi];
  }
  String out;
  serializeJson(doc, out);
  return out;
}

String profileLegacyNameAt(int legacyIndex) {
  if (legacyIndex < 0 || legacyIndex >= legacyCount) return "";
  const String &n = legacyNames[legacyIndex];
  if (findUser(n.c_str()) >= 0) return n;
  char renamed[PROFILE_NAME_MAX_LEN + 1];
  profileMakeOldName(n.c_str(), renamed, sizeof renamed);
  return findUser(renamed) >= 0 ? String(renamed) : String("");
}

static bool checkNewName(const char *name, const char *except, const char **err) {
  if (!name || !*name) { *err = "Name can't be empty"; return false; }
  if (strlen(name) > PROFILE_NAME_MAX_LEN) { *err = "Name longer than 20 characters"; return false; }
  const Profile *other = profileFind(name);
  if (other && !(except && profileNameEquals(other->name, except))) { *err = "A profile with that name already exists"; return false; }
  return true;
}

static bool commit() {
  rebuildEffective();
  return saveStore();
}

bool profileTweak(const char *name, const ProfileTweak &t, const char **err) {
  int bi = findBuiltin(name);
  if (bi >= 0) {
    Profile tmp = baseBuiltins[bi];
    profileApplyTweak(tmp, t); // clamps
    builtinTweaks[bi] = profileGetTweak(tmp);
    builtinTweaked[bi] = true;
    return commit();
  }
  int ui = findUser(name);
  if (ui < 0) { *err = "No such profile"; return false; }
  profileApplyTweak(users[ui], t);
  return commit();
}

bool profileDuplicate(const char *src, const char *newName, const char **err) {
  const Profile *s = profileFind(src);
  if (!s) { *err = "No such profile"; return false; }
  if (userCount >= PROFILE_MAX_COUNT) { *err = "Profile list is full (8 of your own)"; return false; }
  if (!checkNewName(newName, nullptr, err)) return false;
  Profile p = *s;
  p.builtin = false;
  strncpy(p.name, newName, PROFILE_NAME_MAX_LEN);
  p.name[PROFILE_NAME_MAX_LEN] = '\0';
  users[userCount++] = p;
  return commit();
}

bool profileRename(const char *oldName, const char *newName, const char **err) {
  if (findBuiltin(oldName) >= 0) { *err = "Built-in profiles can't be renamed - duplicate it instead"; return false; }
  int ui = findUser(oldName);
  if (ui < 0) { *err = "No such profile"; return false; }
  if (!checkNewName(newName, users[ui].name, err)) return false;
  strncpy(users[ui].name, newName, PROFILE_NAME_MAX_LEN);
  users[ui].name[PROFILE_NAME_MAX_LEN] = '\0';
  return commit();
}

bool profileDeleteUser(const char *name, const char **err) {
  if (findBuiltin(name) >= 0) { *err = "Built-in profiles can't be deleted"; return false; }
  int ui = findUser(name);
  if (ui < 0) { *err = "No such profile"; return false; }
  for (int i = ui; i < userCount - 1; i++) users[i] = users[i + 1];
  userCount--;
  return commit();
}

bool profileReset(const char *name, const char **err) {
  int bi = findBuiltin(name);
  if (bi < 0) { *err = "Only built-in profiles can be reset"; return false; }
  builtinTweaked[bi] = false;
  return commit();
}
```

- [ ] **Step 3: `main.cpp`, replace the profile globals and `applyProfile()`**

1. Add includes after `#include "pump_calibration.h"`:
   ```cpp
   #include "profile_codec.h"
   #include "shot_planner.h"
   ```
2. Delete the line `unsigned long shotAutoStopSec = SHOT_AUTO_STOP_SEC_DEFAULT; // 0 = disabled, persisted`.
3. Replace everything from the comment banner `// Shot profiles (see config.h/profiles.cpp) - a saved profile is just a` through the closing `}` of `void applyProfile(int idx) { ... }` (inclusive) with:
   ```cpp
   // ============================================================================
   // Active shot profile (profiles.cpp, design spec 2026-10-02 §3.1) - selected
   // by NAME and persisted as NVS "profile_name", never by list index, so
   // reordering or deleting can't silently retarget it. Selecting a non-utility
   // profile loads its temperature into brewSetpoint (persisted like a manual
   // edit); utility profiles (Flush/Backflush) leave the brew target alone.
   // ============================================================================
   String activeProfileName = PROFILE_DEFAULT_NAME;
   bool activeShotIsUtility = false;

   bool selectProfile(const char *name) {
     const Profile *p = profileFind(name);
     if (!p) return false;
     activeProfileName = p->name;
     Preferences preferences;
     preferences.begin("gaggia", false);
     preferences.putString("profile_name", activeProfileName);
     if (!p->utility) {
       brewSetpoint = p->temp;
       preferences.putDouble("brew_target", brewSetpoint);
     }
     preferences.end();
     refreshActiveProfileIfChanged();
     return true;
   }
   ```

- [ ] **Step 4: `main.cpp`, replace the ShotStage machinery with the planner**

Replace everything from the comment banner `// Shot stage state machine (2026-08-23, generalized 2026-09-04 for pressure` through the closing `}` of `void startShot() { ... }` (inclusive) with the block below. It keeps the pressure PID, `currentPressure`, `pressureFault` and the pressure-history globals, and drops `ShotStage`, `buildShotStages`, `applyShotStagePumpOutput`, `phaseForStageType`, `tickShotStages`, `pumpBrewPowerPct`, `pumpPreinfusionPowerPct`, `preinfusionFill*`, `preinfusionSoakSec` and `plainDutyPercent`.
```cpp
// ============================================================================
// Shot execution (design spec 2026-10-02 §4.2): the active profile's phase
// list runs in ShotPlanner (pure, shot_planner.cpp), ticked from
// controlTick(). The planner's duty goes to the dimmer; the pressure safety
// cutoffs after it in controlTick() still win. With a usable 0-16 bar sensor
// (PRESSURE_CLOSED_LOOP_AVAILABLE), the pressure PID adds a +/- trim on top
// of the model duty in pressure phases - same profiles, closed loop.
//
// The dimmer has no passive pass-through state: the pump only runs while
// firmware fires the gate - an accepted fail-off tradeoff (bring-up spec §3).
// currentShotPhase is kept for MQTT/legacy reporting only.
// ============================================================================
ShotPhase currentShotPhase = ShotPhase::NONE;

ShotPlanner shotPlanner;
PlannerOutput plannerOut = {};
static unsigned long lastPlannerTickMs = 0;

double pressureSetpoint = 0, pressureInput = 0, pressureOutput = 0;
double pressureKp = PUMP_PRESSURE_KP_DEFAULT, pressureKi = PUMP_PRESSURE_KI_DEFAULT,
       pressureKd = PUMP_PRESSURE_KD_DEFAULT;
PID pressurePID(&pressureInput, &pressureOutput, &pressureSetpoint, pressureKp, pressureKi,
                pressureKd, DIRECT);
static bool pressureClosedLoopActive = false;

float currentPressure = 0.0;
bool pressureFault = false;

// Pressure history (Web UI chart), sampled every TEMP_HISTORY_SAMPLE_INTERVAL_MS.
float pressureHistory[TEMP_HISTORY_LEN] = {0};
int pressureHistoryHead = 0;
int pressureHistoryCount = 0;
// Model estimate + pressure target, sampled together (target < 0 = no
// pressure target at that sample, drawn as a gap).
float estPressureHistory[TEMP_HISTORY_LEN] = {0};
float targetPressureHistory[TEMP_HISTORY_LEN] = {0};
int estHistoryHead = 0;
int estHistoryCount = 0;

static ShotPhase phaseForType(PhaseType t) {
  switch (t) {
    case PhaseType::PRESSURE: return ShotPhase::PRESSURE;
    case PhaseType::FLOW: return ShotPhase::PREINFUSION_ON;
    case PhaseType::SOAK: return ShotPhase::PREINFUSION_OFF;
    default: return ShotPhase::EXTRACTION;
  }
}

static void resetPlannerOut() {
  plannerOut = {};
  plannerOut.phaseIndex = -1;
  plannerOut.phaseName = "";
}

void startShot() {
  if (shotInProgress) return;
  if (pumpCalGetState() == PumpCalState::RUNNING) pumpCalAbort("shot started");
  const Profile *p = profileFind(activeProfileName.c_str());
  if (!p) p = profileFind(PROFILE_DEFAULT_NAME);
  if (!p) return; // no profiles at all - refuse rather than run the pump blind
  shotInProgress = true;
  shotStartMillis = millis();
  lastPlannerTickMs = shotStartMillis;
  shotPeakTemp = currentTemperature;
  activeShotIsUtility = p->utility;
  // Switch Brew to the more aggressive active-brewing gains immediately -
  // see config.h (no bumpless reset on purpose).
  refreshActiveProfileIfChanged();
  PumpModelParams model = {pumpMlPerClick, dimmerGetMainsHz(), (float)p->trimPct};
  shotPlanner.start(*p, model);
  resetPlannerOut();
  currentShotPhase = ShotPhase::NONE;
}
```

- [ ] **Step 5: `main.cpp`, update `stopShot()`**

Replace the body of `void stopShot()` from its first line through the line `shotLogAppend(time(nullptr), durationMs, shotPeakTemp, 0.0, endTemp);` with:
```cpp
  if (!shotInProgress) return;
  shotInProgress = false;
  shotPlanner.stop();
  resetPlannerOut();
  pressureClosedLoopActive = false;
  pressurePID.SetMode(MANUAL);
  dimmerSetPowerPercent(0.0f); // force the pump off
  currentShotPhase = ShotPhase::NONE;
  bool wasUtility = activeShotIsUtility;
  activeShotIsUtility = false;
  unsigned long durationMs = millis() - shotStartMillis;
  // Utility runs (Flush/Backflush) aren't shots - not logged, not counted.
  if (!wasUtility) {
    // 0 = unknown, same convention as weight.
    float endTemp = (currentTemperature > 0) ? currentTemperature : 0.0f;
    shotLogAppend(time(nullptr), durationMs, shotPeakTemp, 0.0, endTemp);
  }
```
Further down in the same function, wrap the shot-count persistence: replace
```cpp
  shotCount++;
  Preferences preferences;
  preferences.begin("gaggia", false);
  preferences.putULong("shot_count", shotCount);
  preferences.end();
```
with
```cpp
  if (wasUtility) return;
  shotCount++;
  Preferences preferences;
  preferences.begin("gaggia", false);
  preferences.putULong("shot_count", shotCount);
  preferences.end();
```

- [ ] **Step 6: `main.cpp`, `setup()` loading**

1. Change `pressurePID.SetOutputLimits(0, 100);` to:
   ```cpp
   // Closed loop (future 0-16 bar sensor) is a +/- trim on the model duty.
   pressurePID.SetOutputLimits(-PUMP_PRESSURE_PID_TRIM_PCT, PUMP_PRESSURE_PID_TRIM_PCT);
   ```
2. Delete these NVS reads in `setup()`: `shotAutoStopSec = ...`, `activeProfileIndex = ...`, `activePreinfusionEnabled/Pulses/OnMs/OffMs = ...`, `activePressureEnabled/RampBar/RampMs/DeclineEnabled/DeclineBar/DeclineMs = ...`, `pumpBrewPowerPct = ...`, `pumpPreinfusionPowerPct = ...`, `preinfusionFillBar = ...`, `preinfusionFillMaxSec = ...` and `preinfusionSoakSec = ...`. In their place (before `preferences.end();`) add:
   ```cpp
   // Legacy values, read ONLY to migrate old profiles (profiles.cpp) - no
   // longer settings of their own (design spec §3.4).
   int legacyBrewPct = preferences.getInt("pump_brew_pct", PUMP_BREW_POWER_PCT_DEFAULT);
   int legacyPiPct = preferences.getInt("pump_pi_pct", PUMP_PREINFUSION_POWER_PCT_DEFAULT);
   int legacyActiveIndex = preferences.getInt("active_profile", -1);
   String savedProfileName = preferences.getString("profile_name", "");
   ```
3. Replace `profilesInit();` with:
   ```cpp
   profilesInit(legacyBrewPct, legacyPiPct);
   ```
   After the existing `pumpCalInit();` line, add:
   ```cpp
   // Active profile by name; first boot after migration maps the old index.
   String startName = savedProfileName;
   if (startName.length() == 0 && legacyActiveIndex >= 0) startName = profileLegacyNameAt(legacyActiveIndex);
   if (!profileFind(startName.c_str())) startName = PROFILE_DEFAULT_NAME;
   activeProfileName = startName;
   resetPlannerOut();
   ```

- [ ] **Step 7: `main.cpp`, `controlTick()` shot loop and history**

1. Inside the `if (now - lastHistorySample >= TEMP_HISTORY_SAMPLE_INTERVAL_MS) {` block, after the `if (!pressureFault) { ... }` sub-block, add:
   ```cpp
       estPressureHistory[estHistoryHead] = shotInProgress ? plannerOut.pressureEst : 0.0f;
       targetPressureHistory[estHistoryHead] =
           (shotInProgress && plannerOut.phaseIndex >= 0 && plannerOut.phaseType == PhaseType::PRESSURE)
               ? plannerOut.phaseTarget
               : -1.0f;
       estHistoryHead = (estHistoryHead + 1) % TEMP_HISTORY_LEN;
       if (estHistoryCount < TEMP_HISTORY_LEN) estHistoryCount++;
   ```
2. Replace everything from the comment `// Shot auto-stop - see config.h/SHOT_AUTO_STOP_SEC_DEFAULT: this ends the` through the closing `}` of the `if (shotInProgress) { if (pressureClosedLoopActive) { ... } else { ... } }` pump-restore block with:
   ```cpp
     // Shot planner (design spec §4.2) - ends the shot itself when its phases
     // or the profile's max_sec run out. Every tick re-commands the duty, so
     // after a safety cut clears (or mains returns) the pump resumes, ramped
     // by the dimmer's slew.
     if (shotInProgress) {
       float dtS = (now - lastPlannerTickMs) / 1000.0f;
       lastPlannerTickMs = now;
       bool sensorUsable = PRESSURE_CLOSED_LOOP_AVAILABLE && !pressureFault;
       plannerOut = shotPlanner.tick(dtS, currentPressure, sensorUsable);
       if (plannerOut.done) {
         Serial.printf("Shot complete: %s finished after %.1f s\n", activeProfileName.c_str(), plannerOut.elapsedS);
         stopShot();
       } else {
         float duty = plannerOut.dutyPct;
         if (sensorUsable && plannerOut.phaseType == PhaseType::PRESSURE) {
           if (!pressureClosedLoopActive) {
             pressureOutput = 0; // trim starts neutral (bumpless)
             pressureInput = currentPressure;
             pressurePID.SetMode(MANUAL);
             pressurePID.SetMode(AUTOMATIC);
             pressureClosedLoopActive = true;
           }
           pressureSetpoint = plannerOut.phaseTarget;
           pressureInput = currentPressure;
           pressurePID.Compute();
           duty += (float)pressureOutput;
         } else if (pressureClosedLoopActive) {
           pressureClosedLoopActive = false;
           pressurePID.SetMode(MANUAL);
         }
         dimmerSetPowerPercent(duty); // clamps to 0..100
         currentShotPhase = phaseForType(plannerOut.phaseType);
       }
     }
   ```
   Leave the safety-ceiling block that follows unchanged, as well as the `pumpCalTick(now);` added in Task 6.

- [ ] **Step 8: `config.h` clean-up**

1. After `#define PUMP_PRESSURE_KD_DEFAULT 0.0`, add:
   ```cpp
   // Closed loop is a +/- correction on top of the pump-model duty (design
   // spec §4.1), not the whole duty - bounded so a bad reading can't swing
   // the pump from 0 to 100.
   #define PUMP_PRESSURE_PID_TRIM_PCT 30
   ```
2. Delete these now-unused defines **and** the comment paragraphs directly attached to them: `PREINFUSION_FILL_BAR_DEFAULT/MIN/MAX`, `PREINFUSION_FILL_MIN_MS`, `PREINFUSION_FILL_MAX_S_DEFAULT/MIN/MAX`, `PREINFUSION_SOAK_S_DEFAULT/MAX`, `PUMP_POWER_PCT_MIN/MAX`, `PRESSURE_RAMP_BAR_DEFAULT`, `PRESSURE_RAMP_MS_DEFAULT`, `PRESSURE_DECLINE_BAR_DEFAULT`, `PRESSURE_DECLINE_MS_DEFAULT`, `PRESSURE_PROFILE_STAGE_COUNT`, `SHOT_AUTO_STOP_SEC_DEFAULT/MIN/MAX`, `PREINFUSION_PULSES_DEFAULT/MAX`, `PREINFUSION_ON_MS_DEFAULT`, `PREINFUSION_OFF_MS_DEFAULT` and `PREINFUSION_PULSE_MS_MIN/MAX`.
3. Keep `PUMP_BREW_POWER_PCT_DEFAULT` and `PUMP_PREINFUSION_POWER_PCT_DEFAULT`, and replace their comment block (the one that starts `// Pump power used by plain-duty shot stages`) with:
   ```cpp
   // LEGACY (pre-2026-10-02) global pump powers - no longer settings. Read
   // from NVS once, only as fallbacks for migrating old pulse profiles into
   // phase profiles (profiles.cpp / profile_codec.cpp).
   ```
4. Verify nothing references the deleted names:
   `git grep -nE "PREINFUSION_(FILL|SOAK|PULSE|ON_MS|OFF_MS)|PRESSURE_(RAMP|DECLINE|PROFILE_STAGE)|SHOT_AUTO_STOP_SEC|PUMP_POWER_PCT_(MIN|MAX)" -- src include`
   Expected: no output.

- [ ] **Step 9: `web.cpp` server side**

1. **Includes / externs.**
   - Add `#include "profile_codec.h"` and `#include "shot_planner.h"` after `#include "pump_calibration.h"`. `profile_codec.h` provides `ProfileTweak`, `profileGetTweak` and `phaseTypeName`.
   - Delete the externs for `activePressureEnabled`, `activePressureRampBar`, `activePressureRampMs`, `activePressureDeclineEnabled`, `activePressureDeclineBar`, `activePressureDeclineMs`, `shotAutoStopSec`, `pumpBrewPowerPct`, `pumpPreinfusionPowerPct`, `preinfusionFillBar`, `preinfusionFillMaxSec`, `preinfusionSoakSec`, `activeProfileIndex`, `activePreinfusionEnabled`, `activePreinfusionPulses/OnMs/OffMs` and `applyProfile`.
   - Add:
     ```cpp
     extern String activeProfileName;
     extern bool selectProfile(const char *name);
     extern PlannerOutput plannerOut;
     extern float estPressureHistory[];
     extern float targetPressureHistory[];
     extern int estHistoryHead;
     extern int estHistoryCount;
     ```
2. **JSON escaping.** Add above `handleStatus()`:
   ```cpp
   // Profile and phase names are user-supplied - escape before splicing into
   // the hand-built /status JSON.
   static String jsonEscape(const String &s) {
     String o;
     o.reserve(s.length() + 2);
     for (size_t i = 0; i < s.length(); i++) {
       char c = s[i];
       if (c == '"' || c == '\\') { o += '\\'; o += c; }
       else if ((unsigned char)c < 0x20) { char b[7]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
       else o += c;
     }
     return o;
   }
   ```
3. **`handleStatus()` snapshot.**
   - Delete the `snapShotAutoStopSec`, `snapPumpBrewPct` / `snapPumpPiPct`, `snapPiFillBar`, `snapPiFillMaxS` / `snapPiSoakS`, `snapActiveProfileIndex`, `snapPiEnabled`, `snapPiPulses` / `OnMs` / `OffMs` and all `snapPress*` lines **except** `snapPressKp`/`Ki`/`Kd`.
   - Add before `unlockState();`:
     ```cpp
     String snapProfileName = activeProfileName;
     const Profile *snapProf = profileFind(activeProfileName.c_str());
     int snapMaxSec = snapProf ? snapProf->maxSec : 0;
     PlannerOutput snapPlan = plannerOut;
     String snapPhaseName = snapPlan.phaseName ? snapPlan.phaseName : "";
     int snapEstCount = estHistoryCount, snapEstHead = estHistoryHead;
     float snapEst[TEMP_HISTORY_LEN], snapTgt[TEMP_HISTORY_LEN];
     for (int i = 0; i < snapEstCount; i++) {
       int idx = (snapEstHead - snapEstCount + i + TEMP_HISTORY_LEN * 2) % TEMP_HISTORY_LEN;
       snapEst[i] = estPressureHistory[idx];
       snapTgt[i] = targetPressureHistory[idx];
     }
     ```
4. **`handleStatus()` JSON.**
   - Delete the lines that emit `shot_auto_stop_sec`, `pump_brew_pct`, `pump_pi_pct`, `pi_fill_bar`, `pi_fill_max_s`, `pi_soak_s`, `active_profile`, `pi_enabled`, `pi_pulses`, `pi_on_ms`, `pi_off_ms`, `press_enabled`, `press_ramp_bar`, `press_ramp_ms`, `press_decline_enabled`, `press_decline_bar` and `press_decline_ms`.
   - After the `shot_phase` block, add:
     ```cpp
     json += ",\"active_profile_name\":\"" + jsonEscape(snapProfileName) + "\"";
     json += ",\"active_max_sec\":" + String(snapMaxSec);
     json += ",\"phase_name\":\"" + jsonEscape(snapPhaseName) + "\"";
     json += ",\"phase_index\":" + String(snapPlan.phaseIndex);
     json += ",\"phase_count\":" + String(snapPlan.phaseCount);
     json += ",\"phase_type\":\"" + String(snapPlan.phaseIndex >= 0 ? phaseTypeName(snapPlan.phaseType) : "") + "\"";
     json += ",\"phase_target\":" + String(snapPlan.phaseTarget, 2);
     json += ",\"pressure_est\":" + String(snapPlan.pressureEst, 2);
     json += ",\"ml_est\":" + String(snapPlan.mlEst, 1);
     ```
   - After the `pressure_history` array, add:
     ```cpp
     json += ",\"est_history\":[";
     for (int i = 0; i < snapEstCount; i++) { if (i > 0) json += ","; json += String(snapEst[i], 2); }
     json += "],\"target_history\":[";
     for (int i = 0; i < snapEstCount; i++) { if (i > 0) json += ","; json += String(snapTgt[i], 2); }
     json += "]";
     ```
5. **`handleUpdate()`.** Delete the `shot_auto_stop_sec` block, the `pump_brew_pct`, `pump_pi_pct`, `pi_fill_bar`, `pi_fill_max_s` and `pi_soak_s` blocks, and the whole `profile_apply` / `profile_save` / `profile_delete` section (from its `// Named shot profiles (profiles.cpp) - three independent actions` comment through the closing `}` of `if (hasArg("profile_delete")) { ... }`). Put this in its place:
   ```cpp
   // Shot profiles (profiles.cpp) - addressed by NAME (design spec §7).
   if (hasArg("profile_select") && !selectProfile(arg("profile_select").c_str())) actionErr = "No such profile";
   if (hasArg("profile_tweak")) {
     String name = arg("profile_tweak");
     const Profile *p = profileFind(name.c_str());
     if (!p) {
       actionErr = "No such profile";
     } else {
       ProfileTweak t = profileGetTweak(*p);
       if (hasArg("temp")) t.temp = arg("temp").toFloat();
       if (hasArg("max_sec")) t.maxSec = (uint16_t)constrain(arg("max_sec").toInt(), 0, 600);
       if (hasArg("pressure_scale")) t.pressureScale = arg("pressure_scale").toFloat();
       if (hasArg("trim_pct")) t.trimPct = (int8_t)constrain(arg("trim_pct").toInt(), -100, 100);
       if (profileTweak(name.c_str(), t, &actionErr) && activeProfileName.equalsIgnoreCase(name)) selectProfile(name.c_str());
     }
   }
   if (hasArg("profile_duplicate")) {
     String newName = hasArg("new_name") ? arg("new_name") : "";
     profileDuplicate(arg("profile_duplicate").c_str(), newName.c_str(), &actionErr);
   }
   if (hasArg("profile_rename")) {
     String oldName = arg("profile_rename");
     String newName = hasArg("new_name") ? arg("new_name") : "";
     if (profileRename(oldName.c_str(), newName.c_str(), &actionErr) && activeProfileName.equalsIgnoreCase(oldName))
       selectProfile(newName.c_str());
   }
   if (hasArg("profile_delete")) {
     String name = arg("profile_delete");
     if (profileDeleteUser(name.c_str(), &actionErr) && activeProfileName.equalsIgnoreCase(name))
       selectProfile(PROFILE_DEFAULT_NAME);
   }
   if (hasArg("profile_reset")) {
     String name = arg("profile_reset");
     if (profileReset(name.c_str(), &actionErr) && activeProfileName.equalsIgnoreCase(name)) selectProfile(name.c_str());
   }
   ```
6. **`/profiles` route.** Replace the handler body with a locked read:
   ```cpp
   server.on("/profiles", AsyncWebRequestMethod::HTTP_GET, [](AsyncWebServerRequest *request) {
     lockState();
     String body = profilesListJson();
     unlockState();
     request->send(200, "application/json", body);
   });
   ```
7. **`/settings_export`.** Delete the `shot_auto_stop_sec`, `pump_brew_pct`, `pump_pi_pct`, `pi_fill_bar`, `pi_fill_max_s` and `pi_soak_s` lines. After `steam_max_safety`, add `q += "&profile_select=" + enc(activeProfileName);`.

- [ ] **Step 10: Build and run the native tests**

Run: `& $pio run -e esp32-s3-devkitc-1`, then `& $pio test -e native`
Expected: both `SUCCESS` / `PASSED`. Any compile error naming a deleted identifier means a reference was missed in Step 3, 4, 6 or 9. Remove that reference; don't re-add the identifier.

- [ ] **Step 11: Bench check (USB, mains off). Covers Review Focus items 1, 2 and 4.**

Flash (`-t upload`), then:
```powershell
$s = curl.exe -s http://gaggia.local/status | ConvertFrom-Json
$s.active_profile_name                     # a migrated profile name (e.g. "Classic") or "Classic 9 bar"
(curl.exe -s http://gaggia.local/profiles | ConvertFrom-Json).name   # 7 built-ins + the 4 migrated
curl.exe -s "http://gaggia.local/update?profile_duplicate=Londinium&new_name=%22%3Cb%3EX%22"  # name "<b>X
(curl.exe -s http://gaggia.local/status | ConvertFrom-Json) | Out-Null   # must still parse
curl.exe -s "http://gaggia.local/update?profile_duplicate=Londinium&new_name=londinium"       # -> "A profile with that name already exists"
curl.exe -s "http://gaggia.local/update?profile_select=%22%3Cb%3EX%22" ; curl.exe -s "http://gaggia.local/update?profile_delete=%22%3Cb%3EX%22"
(curl.exe -s http://gaggia.local/status | ConvertFrom-Json).active_profile_name   # -> "Classic 9 bar" (fallback)
```
Also confirm in the serial log (`tools/serial_capture.py`, run as a self-terminating background job, see AGENTS.md) that you see `profiles: migrated 4 legacy profiles (0 warnings); backup at /profiles.csv.bak`.
Then run a shot with **no mains**: `curl.exe -s "http://gaggia.local/update?shot=start"`. `/status` should show `phase_name` advancing ("Fill" → "Ramp" → "Hold") and `pressure_est` rising after about 3 s. The shot should end on its own at about 35 s.

- [ ] **Step 12: Commit**

```powershell
git add include/profiles.h src/profiles.cpp src/main.cpp src/web.cpp include/config.h
git commit -m "Switch shots to phase profiles driven by the pump-model planner`n`nProfiles are stored by name in /profiles.json (built-ins + 8 user), with the legacy`nfile migrated after a byte-for-byte backup. Removes the ShotStage machine, global`npump powers, fill/soak settings and the global shot auto-stop (now per profile).`n`nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: Web UI: profiles (Now chips + Tune list, tweak, duplicate, rename, delete, reset)

**Files:**
- Modify: `src/web.cpp` (`index_html`: CSS, the Tune-tab profile cards, the profile JS block, the active-profile sync inside `applyStatus`)

**Interfaces:**
- Consumes: `GET /profiles` and `/update?profile_*` (Task 7); `/status.active_profile_name` and `active_max_sec`; the JS helpers `xhr(url, done(status, text))`, `setText`, `setVal` and `showBanner`.
- Produces (used by Task 9): `var activeProfileName` and `markActiveProfile()`.

- [ ] **Step 1: CSS**

After the `.profile-row-actions { ... }` rule, add:
```css
    .profile-row-main { cursor: pointer; flex: 1; }
    .profile-mini { width: 120px; height: 32px; flex: none; margin: 0 var(--sp-2); }
    @media (max-width: 420px) { .profile-mini { display: none; } }
```

- [ ] **Step 2: Replace the Tune-tab profile cards (HTML)**

Replace from `<div class="card">` (the one containing `<div class="tab-section-title">Shot profiles</div>`) through the closing `</div>` of `<div class="card" id="profile_editor_card" hidden>` with:
```html
      <div class="card">
        <div class="tab-section-title">Shot profiles</div>
        <p class="hint">Tap a profile to use it. Pressure and flow are estimates from the calibrated pump model until a 0&ndash;16 bar sensor is fitted. Pump trim: negative if shots run slow, positive if they run fast.</p>
        <div id="profile_list_body"></div>
      </div>

      <div class="card" id="profile_tweak_card" hidden>
        <div class="tab-section-title" id="profile_tweak_title">Tweak</div>
        <form onsubmit="submitProfileTweak(event)">
          <input type="hidden" id="input_tweak_name" value="">
          <div class="field-row">
            <div class="field" id="tweak_temp_field"><label for="input_tweak_temp">Temperature (&deg;C)</label><input type="number" step="0.5" min="80" max="100" id="input_tweak_temp"></div>
            <div class="field"><label for="input_tweak_max_sec">Max shot time (s)</label><input type="number" step="1" min="5" max="120" id="input_tweak_max_sec"></div>
          </div>
          <div class="field-row">
            <div class="field"><label for="input_tweak_scale">Pressure scale</label><input type="number" step="0.05" min="0.7" max="1.1" id="input_tweak_scale"></div>
            <div class="field"><label for="input_tweak_trim">Pump trim (%)</label><input type="number" step="1" min="-20" max="20" id="input_tweak_trim"></div>
          </div>
          <button type="submit" id="profile_tweak_submit" class="submit">Save</button>
          <button type="button" onclick="closeProfileTweak()" class="btn-secondary">Cancel</button>
        </form>
      </div>
```

- [ ] **Step 3: Replace the profile JS block**

Replace from the comment `// Named shot profiles (Now-tab quick-select chips + Tune-tab management list)` through the closing `}` of `function submitProfileForm(ev) { ... }` with:
```js
// ============================================================================
// Shot profiles - by NAME (design spec 2026-10-02 §7). All profile data goes
// into the DOM via textContent/createElement(NS) only - names are user input.
// ============================================================================
var profilesCache = [];
var activeProfileName = "";
var lastProfilesJson = "";

function profileTotalSec(p) {
  var t = 0;
  p.phases.forEach(function (ph) { t += ph.duration_s; });
  return Math.min(t, p.max_sec);
}

function profileSummary(p) {
  if (p.utility) return "Utility · " + profileTotalSec(p).toFixed(0) + "s";
  var peak = 0;
  p.phases.forEach(function (ph) { if (ph.type === "pressure") peak = Math.max(peak, ph.target * p.pressure_scale); });
  var s = p.temp.toFixed(1) + "°C · " + p.phases.length + " phases · " + profileTotalSec(p).toFixed(0) + "s";
  if (peak > 0) s += " · peak " + peak.toFixed(1) + " bar";
  if (p.trim_pct) s += " · trim " + (p.trim_pct > 0 ? "+" : "") + p.trim_pct + "%";
  if (p.tweaked) s += " · tweaked";
  return s;
}

function easeJs(curve, x) {
  if (curve === "linear") return x;
  if (curve === "ease-in") return x * x * x;
  if (curve === "ease-out") return 1 - Math.pow(1 - x, 3);
  if (curve === "ease-in-out") return x < 0.5 ? 4 * x * x * x : 1 - Math.pow(-2 * x + 2, 3) / 2;
  return 1;
}

// Mini phase chart: each phase's target over time, normalised per unit
// (bar/10, ml/s/10, %/100) so mixed phases share one axis; colour = type.
var SVG_NS = "http://www.w3.org/2000/svg";
var PHASE_COLORS = { pressure: "#d98c3f", flow: "#3f8cd9", power: "#9a8b7e", soak: "#4a3f37" };
function phaseNorm(ph, v, scale) {
  if (ph.type === "pressure") return (v * scale) / 10;
  if (ph.type === "flow") return v / 10;
  if (ph.type === "power") return v / 100;
  return 0;
}
function buildProfileMiniChart(p) {
  var W = 120, H = 32;
  var svg = document.createElementNS(SVG_NS, "svg");
  svg.setAttribute("viewBox", "0 0 " + W + " " + H);
  svg.setAttribute("class", "profile-mini");
  svg.setAttribute("aria-hidden", "true");
  var total = 0;
  p.phases.forEach(function (ph) { total += ph.duration_s; });
  if (total <= 0) return svg;
  var t = 0, prevEnd = 0;
  p.phases.forEach(function (ph) {
    var end = phaseNorm(ph, ph.target, p.pressure_scale);
    var start = ph.start !== undefined ? phaseNorm(ph, ph.start, p.pressure_scale) : prevEnd;
    if (ph.type === "soak") { start = 0; end = 0; }
    var ramp = (ph.curve && ph.curve !== "instant") ? Math.min(ph.ramp_s || 0, ph.duration_s) : 0;
    var pts = [];
    if (ramp > 0) {
      for (var i = 0; i <= 8; i++) pts.push([t + (i / 8) * ramp, start + (end - start) * easeJs(ph.curve, i / 8)]);
    } else {
      pts.push([t, end]);
    }
    pts.push([t + ph.duration_s, end]);
    var line = document.createElementNS(SVG_NS, "polyline");
    line.setAttribute("points", pts.map(function (q) {
      return ((q[0] / total) * W).toFixed(1) + "," + (H - 2 - Math.min(1, q[1]) * (H - 4)).toFixed(1);
    }).join(" "));
    line.setAttribute("fill", "none");
    line.setAttribute("stroke", PHASE_COLORS[ph.type] || "#888");
    line.setAttribute("stroke-width", "2");
    svg.appendChild(line);
    t += ph.duration_s;
    prevEnd = end;
  });
  return svg;
}

function markActiveProfile() {
  var chips = document.querySelectorAll("#profile_chip_row .btn-preset");
  for (var i = 0; i < chips.length; i++) chips[i].classList.toggle("active", chips[i].dataset.name === activeProfileName);
  var rows = document.querySelectorAll("#profile_list_body .profile-row");
  for (var j = 0; j < rows.length; j++) rows[j].classList.toggle("active", rows[j].dataset.name === activeProfileName);
}

function profileActionDone(status, text) {
  if (status !== 200) alert(text || "Request failed");
  lastProfilesJson = "";
  fetchProfiles();
}

function selectProfileByName(name) {
  activeProfileName = name;
  markActiveProfile();
  xhr("/update?profile_select=" + encodeURIComponent(name), profileActionDone);
}

function renderProfileChips() {
  var row = document.getElementById("profile_chip_row");
  if (!row) return;
  row.textContent = "";
  profilesCache.forEach(function (p) {
    var btn = document.createElement("button");
    btn.type = "button";
    btn.className = "btn-preset";
    btn.dataset.name = p.name;
    btn.textContent = p.name;
    btn.addEventListener("click", function () { selectProfileByName(p.name); });
    row.appendChild(btn);
  });
  markActiveProfile();
}

function addRowButton(parent, label, cls, onClick) {
  var b = document.createElement("button");
  b.type = "button";
  b.className = "btn-chip-sm" + (cls ? " " + cls : "");
  b.textContent = label;
  b.addEventListener("click", function (e) { e.stopPropagation(); onClick(b); });
  parent.appendChild(b);
  return b;
}

function renderProfileList() {
  var body = document.getElementById("profile_list_body");
  if (!body) return;
  body.textContent = "";
  profilesCache.forEach(function (p) {
    var row = document.createElement("div");
    row.className = "profile-row";
    row.dataset.name = p.name;
    var main = document.createElement("div");
    main.className = "profile-row-main";
    var nameEl = document.createElement("b");
    nameEl.textContent = p.name + (p.builtin ? "" : " ★");
    var meta = document.createElement("span");
    meta.textContent = profileSummary(p);
    main.appendChild(nameEl);
    main.appendChild(meta);
    main.addEventListener("click", function () { selectProfileByName(p.name); });
    var actions = document.createElement("div");
    actions.className = "profile-row-actions";
    addRowButton(actions, "Tweak", "", function () { openProfileTweak(p); });
    addRowButton(actions, "Copy", "", function () {
      var n = prompt("Name for the copy (max 20 characters):", (p.name + " copy").slice(0, 20));
      if (!n) return;
      xhr("/update?profile_duplicate=" + encodeURIComponent(p.name) + "&new_name=" + encodeURIComponent(n), profileActionDone);
    });
    if (p.builtin && p.tweaked) {
      addRowButton(actions, "Reset", "", function () { xhr("/update?profile_reset=" + encodeURIComponent(p.name), profileActionDone); });
    }
    if (!p.builtin) {
      addRowButton(actions, "Rename", "", function () {
        var n = prompt("New name (max 20 characters):", p.name);
        if (!n || n === p.name) return;
        xhr("/update?profile_rename=" + encodeURIComponent(p.name) + "&new_name=" + encodeURIComponent(n), profileActionDone);
      });
      addRowButton(actions, "Delete", "danger", function (btn) { armDeleteProfile(btn, p.name); });
    }
    row.appendChild(main);
    row.appendChild(buildProfileMiniChart(p));
    row.appendChild(actions);
    body.appendChild(row);
  });
  markActiveProfile();
}

function fetchProfiles() {
  xhr("/profiles", function (status, text) {
    if (status !== 200 || text === lastProfilesJson) return;
    lastProfilesJson = text;
    try { profilesCache = JSON.parse(text); } catch (err) { return; }
    renderProfileChips();
    renderProfileList();
  });
}
fetchProfiles();

function openProfileTweak(p) {
  document.getElementById("input_tweak_name").value = p.name;
  document.getElementById("profile_tweak_title").textContent = "Tweak \"" + p.name + "\"";
  document.getElementById("tweak_temp_field").hidden = !!p.utility;
  document.getElementById("input_tweak_temp").value = p.temp;
  document.getElementById("input_tweak_max_sec").value = p.max_sec;
  document.getElementById("input_tweak_scale").value = p.pressure_scale;
  document.getElementById("input_tweak_trim").value = p.trim_pct;
  var card = document.getElementById("profile_tweak_card");
  card.hidden = false;
  card.scrollIntoView({ behavior: "smooth", block: "center" });
}

function closeProfileTweak() { document.getElementById("profile_tweak_card").hidden = true; }

function submitProfileTweak(ev) {
  ev.preventDefault();
  var q = "profile_tweak=" + encodeURIComponent(document.getElementById("input_tweak_name").value) +
    "&temp=" + encodeURIComponent(document.getElementById("input_tweak_temp").value) +
    "&max_sec=" + encodeURIComponent(document.getElementById("input_tweak_max_sec").value) +
    "&pressure_scale=" + encodeURIComponent(document.getElementById("input_tweak_scale").value) +
    "&trim_pct=" + encodeURIComponent(document.getElementById("input_tweak_trim").value);
  var btn = document.getElementById("profile_tweak_submit");
  btn.disabled = true;
  btn.textContent = "Saving…";
  xhr("/update?" + q, function (status, text) {
    profileActionDone(status, text);
    btn.textContent = status === 200 ? "Saved" : "Save failed";
    setTimeout(function () {
      btn.disabled = false;
      btn.textContent = "Save";
      if (status === 200) closeProfileTweak();
    }, 900);
  });
}

// Arm-then-confirm delete (no native confirm()): first click arms for 4 s,
// second click deletes; arming another disarms the first.
var armedDeleteBtn = null, armedDeleteTimer = null;
function disarmDeleteProfile() {
  if (armedDeleteTimer) { clearTimeout(armedDeleteTimer); armedDeleteTimer = null; }
  if (armedDeleteBtn) { armedDeleteBtn.textContent = "Delete"; armedDeleteBtn.classList.remove("armed"); armedDeleteBtn = null; }
}
function armDeleteProfile(btn, name) {
  if (btn === armedDeleteBtn) {
    disarmDeleteProfile();
    xhr("/update?profile_delete=" + encodeURIComponent(name), profileActionDone);
    return;
  }
  disarmDeleteProfile();
  armedDeleteBtn = btn;
  btn.textContent = "Confirm?";
  btn.classList.add("armed");
  armedDeleteTimer = setTimeout(disarmDeleteProfile, 4000);
}
```

- [ ] **Step 4: Sync the active profile in `applyStatus`**

Replace:
```js
  if (json.active_profile !== activeProfileIndex) {
    activeProfileIndex = json.active_profile;
    markActiveProfile();
  }
```
with:
```js
  if (typeof json.active_profile_name === "string" && json.active_profile_name !== activeProfileName) {
    activeProfileName = json.active_profile_name;
    markActiveProfile();
  }
```

- [ ] **Step 5: Check that no old profile code is left**

Run: `git grep -nE "activeProfileIndex|drawProfilePreview|piSummary|profile_apply|profile_save|input_profile_" -- src/web.cpp`
Expected: no output.

- [ ] **Step 6: Build, flash, and check in a browser (USB, mains off)**

`& $pio run -e esp32-s3-devkitc-1 -t upload`, then open `http://gaggia.local/#tune` and check:
- 11 rows, each with a mini chart;
- tapping Londinium highlights it and its Now chip;
- **Tweak** changes the temperature and the summary updates;
- **Copy** creates a ★ row;
- **Rename** works on the ★ row;
- **Delete** asks "Confirm?" and then removes the row;
- **Reset** appears only on tweaked built-ins;
- a name containing `<b>` displays literally.

- [ ] **Step 7: Commit**

```powershell
git add src/web.cpp
git commit -m "Web UI: name-based phase profiles with mini charts, tweak/copy/rename/delete/reset`n`nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 9: Web UI: Now tab telemetry (est. bar, phase, water, mains/calibration banners, chart)

**Files:**
- Modify: `src/web.cpp` (`index_html`: Now-tab HTML, `drawPressureSparkline` → `drawPressureChart`, the `applyStatus` pressure/pump/shot-label sections)

**Interfaces:**
- Consumes: `/status` fields `pressure_est`, `ml_est`, `phase_*`, `est_history`, `target_history`, `pressure_history`, `pump_mains`, `mains_hz`, `pump_calibrated`, `pump_cal_state`, `active_profile_name`, `active_max_sec` and `press_available` (Tasks 6 and 7); `activeProfileName` (Task 8).
- Produces: nothing new for later tasks.

- [ ] **Step 1: Now-tab HTML**

1. After `<div id="pump_banner" class="banner banner-warn"></div>`, add:
   ```html
      <div id="cal_banner" class="banner banner-info">Pump not calibrated &mdash; shots use a default pump model.&nbsp;<a href="#settings" onclick="showTab('settings')">Calibrate</a></div>
   ```
2. In the `stat-grid`, after the Pump `stat-tile` `</div>`, add:
   ```html
          <div class="stat-tile">
            <span class="stat-label">Water (est.)</span>
            <span class="stat-value"><span id="ml_est" class="skeleton">--</span><small>ml</small></span>
          </div>
   ```
3. Change `<span>Pressure &middot; last 2 min</span>` to `<span>Pressure (est.) &middot; last 2 min</span>`.

- [ ] **Step 2: Replace `drawPressureSparkline`**

Replace the whole `function drawPressureSparkline(data, target) { ... }` with:
```js
// Pressure chart: the model estimate (or the real reading once the closed
// loop exists) as the main line, the raw sensor reading as a thin line
// while it can't read brew pressure, and the profile's pressure target
// dashed (target < 0 = no pressure target at that sample -> gap).
function drawPressureChart(est, sensor, target, closedLoop) {
  var canvas = document.getElementById("pressure_chart");
  if (!canvas) return;
  var main = closedLoop ? sensor : est;
  if (!main || main.length < 2) return;
  var ctx = canvas.getContext("2d");
  var w = canvas.width, h = canvas.height;
  ctx.clearRect(0, 0, w, h);
  var max = 10;
  function y(v) { return h - (Math.min(max, Math.max(0, v)) / max) * (h - 6) - 3; }
  function line(data, color, width, dashed, gapsBelowZero) {
    if (!data || data.length < 2) return;
    ctx.beginPath();
    var pen = false;
    data.forEach(function (v, i) {
      var x = (i / (data.length - 1)) * w;
      if (gapsBelowZero && v < 0) { pen = false; return; }
      if (!pen) { ctx.moveTo(x, y(v)); pen = true; } else ctx.lineTo(x, y(v));
    });
    ctx.setLineDash(dashed ? [5, 4] : []);
    ctx.strokeStyle = color;
    ctx.lineWidth = width;
    ctx.lineJoin = "round";
    ctx.stroke();
    ctx.setLineDash([]);
  }
  line(target, "#d98c3f", 1.5, true, true);
  if (!closedLoop) line(sensor, "#6b5d52", 1, false, false);
  line(main, "#3f8cd9", 2.5, false, false);
}
```

- [ ] **Step 3: `applyStatus`: pressure, telemetry and phase markers**

Replace from `trackPhaseMarkers(json.shot_phase, json.shot_in_progress);` through the closing `}` of the `if (pCap) { ... }` block with:
```js
  trackPhaseMarkers(json.phase_index, json.shot_in_progress);
  drawSparkline(json.history, hasTarget ? target : null);
  var closedLoop = json.press_available === true && !json.pressure_fault;
  drawPressureChart(json.est_history, json.pressure_history, json.target_history, closedLoop);
  var pLabel = document.getElementById("pressure_label");
  if (pLabel) {
    pLabel.textContent = json.pressure_fault ? "sensor fault"
      : "sensor " + (json.pressure_max_bar && json.pressure >= json.pressure_max_bar
        ? json.pressure_max_bar.toFixed(0) + "+" : json.pressure.toFixed(1)) + " bar";
  }
  setText("ml_est", json.shot_in_progress ? json.ml_est.toFixed(0) : "--");

  var tempPct = (temp > 0 && hasTarget) ? clamp((temp / target) * 100) : 0;
  var ring = document.getElementById("temp_ring_fill");
  var RING_CIRCUMFERENCE = 603;
  ring.style.strokeDashoffset = RING_CIRCUMFERENCE * (1 - tempPct / 100);
  ring.classList.remove("heating", "ready", "over");
  if (!json.fault && temp > 0 && hasTarget) {
    var READY_MARGIN_C = 1.0;
    if (temp < target - READY_MARGIN_C) ring.classList.add("heating");
    else if (temp > target + READY_MARGIN_C) ring.classList.add("over");
    else ring.classList.add("ready");
  }

  // Pressure gauge: the model estimate during a shot (no usable sensor),
  // the real reading once the closed loop exists. Goal = the current
  // pressure-phase target.
  var PRESSURE_GAUGE_MAX_BAR = 12;
  var showPressure = closedLoop || json.shot_in_progress;
  var pressureVal = closedLoop ? json.pressure : (json.shot_in_progress ? json.pressure_est : 0);
  var pressureGoal = (json.shot_in_progress && json.phase_type === "pressure") ? json.phase_target : null;
  var pRing = document.getElementById("pressure_ring_fill");
  if (pRing) {
    pRing.style.strokeDashoffset = RING_CIRCUMFERENCE * (1 - clamp((pressureVal / PRESSURE_GAUGE_MAX_BAR) * 100) / 100);
    pRing.classList.remove("heating", "ready", "over");
    if (pressureGoal !== null) {
      var PRESSURE_READY_MARGIN_BAR = 1.0;
      if (pressureVal < pressureGoal - PRESSURE_READY_MARGIN_BAR) pRing.classList.add("heating");
      else if (pressureVal > pressureGoal + PRESSURE_READY_MARGIN_BAR) pRing.classList.add("over");
      else pRing.classList.add("ready");
    }
  }
  setText("pressure_gauge_val", showPressure ? pressureVal.toFixed(1) : "--");
  var pCap = document.getElementById("pressure_target_caption");
  if (pCap) {
    pCap.textContent = (closedLoop ? "" : "est. · ") +
      (pressureGoal !== null ? "target " + pressureGoal.toFixed(1) + " bar" : "pressure");
  }
```

- [ ] **Step 4: `applyStatus`: remove the old pump-setting sync and the zero-cross rate**

1. Delete the lines `setVal("input_shot_auto_stop", ...)`, the five `if (typeof json.pump_brew_pct ... )` / `pump_pi_pct` / `pi_fill_bar` / `pi_fill_max_s` / `pi_soak_s` lines, and the `var piFill = ...; if (piFill) ...` pair.
2. Delete the `if (typeof json.dimmer_zc_count === "number") { ... }` block, plus the `var lastZcCount`, `var lastZcAt` and `var zcHz` declarations.
3. Replace from `var brewSwitchOff = json.shot_in_progress && zcHz < 20 && lastZcCount !== null;` through the end of the `shotAutoStopSec = json.shot_auto_stop_sec;` line with:
   ```js
   var pumpWanted = json.shot_in_progress || json.pump_cal_state === "running";
   if (pumpWanted && json.pump_mains === "absent") {
     showBanner("pump_banner", true, "No mains to the pump — turn the machine's Brew switch on.");
   } else if (pumpWanted && json.pump_mains === "unstable") {
     showBanner("pump_banner", true, "Mains zero-cross signal looks unstable (" + json.mains_hz.toFixed(1) + " Hz).");
   } else {
     showBanner("pump_banner", false);
   }
   showBanner("cal_banner", json.pump_calibrated === false);

   var shotLabel = document.getElementById("shot_auto_stop_label");
   if (json.shot_in_progress && json.phase_index >= 0) {
     var unit = json.phase_type === "pressure" ? " bar" : json.phase_type === "flow" ? " ml/s" : json.phase_type === "power" ? "%" : "";
     var tgt = json.phase_type === "soak" ? "pump off" : json.phase_target.toFixed(1) + unit;
     shotLabel.textContent = json.phase_name + " · " + (json.phase_index + 1) + "/" + json.phase_count + " · " + tgt;
   } else {
     shotLabel.textContent = (json.active_profile_name || "--") + " · stops by " + json.active_max_sec + "s";
   }
   shotAutoStopSec = json.active_max_sec;
   ```
4. Delete `"dimmer_zc_count"` from the server too: in `handleStatus()`, remove the line `json += ",\"dimmer_zc_count\":" + String(snapDimmerZcCount);` (`zc_count` replaces it), and update the comment above `snapDimmerZcCount` to `// Zero-cross count (dimmer health), exposed as zc_count.`
5. Remove the word "Temporary" from the comments on `dimmerGetZcCount` in `dimmer.h`/`.cpp` if any remain.

- [ ] **Step 5: Check, build and look at it**

`git grep -nE "zcHz|lastZcCount|dimmer_zc_count|drawPressureSparkline|shot_auto_stop_sec|pump_brew_pct|pi_fill" -- src` → expected: no output.
Flash, open `http://gaggia.local/` (mains off), and check:
- the "Pump not calibrated" banner shows;
- Start Shot shows "Fill · 1/3 · 4.0 ml/s", then "Ramp · 2/3";
- the pressure gauge shows est. bar climbing after about 3 s;
- the chart shows a blue est line with an orange dashed target;
- "No mains to the pump" shows during the shot (mains is off);
- Water (est.) counts up.

- [ ] **Step 6: Commit**

```powershell
git add src/web.cpp include/dimmer.h src/dimmer.cpp
git commit -m "Web UI: Now tab shows est. bar, phase, water, mains and calibration state`n`nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 10: Web UI: Settings calibration card; remove the old pump settings

**Files:**
- Modify: `src/web.cpp` (`index_html`: Settings → Brewing cards, the pump-PID hint, the backup hint, calibration JS)

**Interfaces:**
- Consumes: `/update?pump_cal=…`, `pump_cal_redo`, `pump_cal_grams` and `/status.pump_cal_*`, `pump_ml_per_click`, `pump_calibrated`, `pump_mains` (Task 6).
- Produces: nothing for later tasks.

- [ ] **Step 1: Replace the two Brewing cards**

In the Settings view, replace from `<div class="card">` (containing `<div class="tab-section-title">Shot auto-stop</div>`) through the closing `</div>` of the "Pump power" card (just before `<div class="group-title">Power</div>`) with:
```html
      <div class="card" id="pump_cal_card">
        <div class="tab-section-title">Pump calibration</div>
        <p class="hint">Teaches the pump model how much water your pump moves. Remove the portafilter, put a cup on a scale under the group and tare it, and turn the Brew switch on. Each run pumps at full power for 10 s. Three runs are averaged; a run more than 10% off the others has to be redone.</p>
        <div class="metric-row"><span>Current value</span><b><span id="pump_cal_current">--</span></b></div>
        <div id="pump_cal_runs"></div>
        <p id="pump_cal_status" class="hint" aria-live="polite"></p>
        <p id="pump_cal_error" class="hint" style="color:var(--danger)" aria-live="assertive"></p>
        <div id="pump_cal_grams_row" hidden>
          <div class="field">
            <label for="input_pump_cal_grams">Grams in the cup</label>
            <input type="number" step="0.1" min="1" max="500" id="input_pump_cal_grams">
          </div>
          <button type="button" onclick="submitCalGrams()" class="submit">Record</button>
        </div>
        <button type="button" onclick="calAction('run')" id="btn_pump_cal_run" class="submit">Start run</button>
        <button type="button" onclick="calAction('stop')" id="btn_pump_cal_stop" class="btn-secondary" hidden>Stop</button>
        <button type="button" onclick="calAction('save')" id="btn_pump_cal_save" class="submit" hidden>Save calibration</button>
        <button type="button" onclick="calAction('cancel')" class="btn-secondary">Clear runs</button>
      </div>
```

- [ ] **Step 2: Update the two hints**

1. Pump pressure PID card: replace `<p class="hint">Used only during a profile's pressure ramp/decline stages. Pre-infusion pulses stay on/off.</p>` with `<p class="hint">Used once a 0&ndash;16 bar sensor is fitted: adds a correction on top of the pump model during pressure phases.</p>`.
2. Backup card: replace `Brew/Steam/schedule/MQTT settings. Named shot profiles live in a separate file and are not included here.` with `Brew/Steam/schedule/MQTT settings, the active profile and pump calibration. Your own shot profiles live in a separate file and are not included.`

- [ ] **Step 3: Calibration JS**

Add right after `function setSchedEnabled(i, checked) { ... }`:
```js
// Pump calibration card (design spec §6). Errors come back as 409 + text.
function calDone(status, text) { setText("pump_cal_error", status === 200 ? "" : (text || "Request failed")); }
function calAction(a) { xhr("/update?pump_cal=" + a, calDone); }
function calRedo(n) { xhr("/update?pump_cal_redo=" + n, calDone); }
function submitCalGrams() {
  var input = document.getElementById("input_pump_cal_grams");
  xhr("/update?pump_cal_grams=" + encodeURIComponent(input.value), function (s, t) {
    calDone(s, t);
    if (s === 200) input.value = "";
  });
}
function applyCalStatus(json) {
  if (typeof json.pump_cal_state !== "string") return;
  setText("pump_cal_current", json.pump_ml_per_click.toFixed(3) + " ml/stroke" + (json.pump_calibrated ? "" : " (default)"));
  var vals = json.pump_cal_runs || [];
  var n = 0, mean = 0;
  vals.forEach(function (v) { if (v !== null) { n++; mean += v; } });
  mean = n ? mean / n : 0;
  var st = json.pump_cal_state;
  var runsEl = document.getElementById("pump_cal_runs");
  runsEl.textContent = "";
  for (var i = 0; i < 3; i++) {
    var v = vals[i] === undefined ? null : vals[i];
    var row = document.createElement("div");
    row.className = "metric-row";
    var label = document.createElement("span");
    label.textContent = "Run " + (i + 1);
    var val = document.createElement("b");
    var off = v !== null && n === 3 && mean > 0 && Math.abs(v - mean) / mean > 0.10;
    if (v !== null) val.textContent = v.toFixed(3) + (off ? " ⚠ redo" : "");
    else if (json.pump_cal_run === i) val.textContent = st === "running" ? "pumping…" : "enter grams";
    else val.textContent = "—";
    row.appendChild(label);
    row.appendChild(val);
    if (v !== null && (st === "idle" || st === "aborted")) {
      var b = document.createElement("button");
      b.type = "button";
      b.className = "btn-chip-sm";
      b.textContent = "Redo";
      b.addEventListener("click", (function (k) { return function () { calRedo(k); }; })(i + 1));
      row.appendChild(b);
    }
    runsEl.appendChild(row);
  }
  var msg = st === "running" ? "Run " + (json.pump_cal_run + 1) + ": pumping " + Math.floor(json.pump_cal_elapsed_ms / 1000) + " / 10 s"
    : st === "await_grams" ? "Weigh the cup and enter the grams for run " + (json.pump_cal_run + 1) + "."
    : st === "aborted" ? "Run aborted: " + json.pump_cal_reason + ". Start it again."
    : n === 3 ? "All 3 runs done — Save, or Redo one."
    : json.pump_mains !== "ok" ? "Turn the Brew switch on to start a run."
    : "";
  setText("pump_cal_status", msg);
  document.getElementById("pump_cal_grams_row").hidden = st !== "await_grams";
  var runBtn = document.getElementById("btn_pump_cal_run");
  runBtn.hidden = st === "running" || st === "await_grams" || n === 3;
  runBtn.disabled = json.pump_mains !== "ok";
  document.getElementById("btn_pump_cal_stop").hidden = st !== "running";
  document.getElementById("btn_pump_cal_save").hidden = !(n === 3 && (st === "idle" || st === "aborted"));
}
```
In `applyStatus`, add `applyCalStatus(json);` immediately after the line `showBanner("cal_banner", json.pump_calibrated === false);` from Task 9.

- [ ] **Step 4: Check, build and look at it**

`git grep -nE "input_shot_auto_stop|input_pump_brew_pct|input_pump_pi_pct|pi_fill_fields|input_pi_" -- src/web.cpp` → expected: no output.
Flash, then open `http://gaggia.local/#settings` (mains off):
- "Start run" is disabled;
- the status reads "Turn the Brew switch on to start a run.";
- "Current value" reads `0.213 ml/stroke (default)`.

- [ ] **Step 5: Commit**

```powershell
git add src/web.cpp
git commit -m "Web UI: pump calibration card replaces global pump power and auto-stop settings`n`nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 11: Docs

**Files:**
- Modify: `AGENTS.md`, `HARDWARE_ROADMAP.md`

- [ ] **Step 1: AGENTS.md section 4 (Software / architecture)**

1. Add these entries to the **Files** list:
   - `src/pump_model.cpp` — the pure EP5 + reference-puck open-loop model;
   - `src/pump_cal_math.cpp` and `src/pump_calibration.cpp` — 3-run scale calibration, owning NVS `pump_mlpc`;
   - `src/profile_codec.cpp` — built-ins, validation, legacy migration;
   - `src/shot_planner.cpp` — the phase sequencer;
   - `src/profiles.cpp` — the `/profiles.json` store;
   - `test/` — Unity tests, `pio test -e native`, and the host gcc requirement.
2. Add a new subsection **"Phase profiles + open-loop pump model (2026-10-02)"** that summarises spec §§3–6 in 10–15 lines:
   - profiles are addressed by name;
   - the 7 built-ins;
   - Tweak / Copy / Rename / Delete / Reset;
   - the per-profile `max_sec` replaces the old global auto-stop;
   - `P_est` is an estimate;
   - flipping `PRESSURE_CLOSED_LOOP_AVAILABLE` turns the pressure PID into a ±30% trim;
   - the calibration procedure;
   - the dimmer hardening: 1500 µs gptimer gate hold, mains `ok`/`absent`/`unstable`, `fired_cycles`, +250%/s slew.
3. In section 5, add `& $pio test -e native` under the build commands.
4. Delete the remaining sentences that call the zero-cross counter a "temporary diagnostic". Search for `Temporary`, `dimmer_zc_count` and `Remove this diagnostic`.

- [ ] **Step 2: AGENTS.md change log**

Add at the top of section 10:
```markdown
### 2026-10-02 — Claude Code (Opus 5.5) — Phase profiles, open-loop pump model, dimmer hardening, pump calibration

- **Research**: read Gaggiuino (`release/stm32-blackpill`) and GaggiMate (`master`) source. Both use 0-12/16 bar 0.5-4.5 V sensors via ADS1115 + Kalman filtering, and PSM pulse-skip pump drive. Their pump models disagree ~2x at 9 bar (Gaggiuino EP5 curve -> ~55-75% duty to hold 9 bar; GaggiMate linear 10.2 ml/s@1 bar / 5.5@9 -> ~27-36%) - hence a per-machine calibration instead of a fixed %. Peak targets are 8-9 bar with declines/flow phases, not flat 9 bar.
- **User decisions**: keep the 0-5 bar sensor for now (closed loop must switch on later with no profile changes); phase-% from a model fitted by a 3-run scale calibration with a 10% outlier check; built-in profiles + simple tweaks, no full phase editor yet; harden the dimmer across the board.
- **Built**:
  - pure units `pump_model`, `pump_cal_math`, `profile_codec`, `shot_planner` with Unity tests (`pio test -e native`);
  - `/profiles.json` store addressed by name;
  - 7 built-ins: Classic 9 bar, Londinium, Lever, IUIUIU Classic, Blooming, Flush, Backflush;
  - migration of the 4 legacy profiles after a byte-for-byte backup to `/profiles.csv.bak`;
  - the planner runs in `controlTick()`;
  - a 3-run calibration card;
  - Now-tab est. bar / phase / water / mains banners.
- **Dimmer**: gate raised in the ZC ISR and held 1500 µs, dropped by a gptimer alarm (was a 100 µs pulse 300 µs after ZC - risk of lost strokes on the inductive pump); mains Hz auto-detect + 100 ms zero-cross watchdog; permanent `zc_count`/`fired_cycles`; +250 %/s slew (cuts stay instant).
- **Removed settings**: global brew/pre-infusion pump %, fill/soak settings, global shot auto-stop (now each profile's `max_sec`).
- **Unused NVS keys left in place**, for a future cleanup: `shot_auto_stop`, `pump_brew_pct`, `pump_pi_pct` (read once for migration), `pi_fill_bar`, `pi_fill_max_s`, `pi_soak_s`, `pi_enabled`, `pi_pulses`, `pi_on_ms`, `pi_off_ms`, `press_en`, `press_ramp_bar`, `press_ramp_ms`, `press_dec_en`, `press_dec_bar`, `press_dec_ms`, `active_profile` (read once to map to a name).
- **Not yet verified on the real machine**: see the plan's Task 12 checklist.
```

- [ ] **Step 3: HARDWARE_ROADMAP.md item 8**

Under item 8, add a short **"Firmware status (2026-10-02)"** paragraph:
- the dimmer now drives a gate pulse held 1500 µs from each zero-cross edge, dropped by a hardware timer;
- the pump only runs during a shot or a calibration run;
- the calibration procedure: portafilter off, tared cup on the scale, Brew switch on, Settings → Pump calibration, 3 × 10 s runs at full power, enter the grams each time;
- the 0–5 bar sensor is display-only until a 0–16 bar part replaces it.

- [ ] **Step 4: Commit**

```powershell
git add AGENTS.md HARDWARE_ROADMAP.md
git commit -m "Docs: phase profiles, pump model, calibration and dimmer hardening`n`nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 12: Supervised live verification (user at the machine)

**Mains-side.** Follow AGENTS.md §6. The user stays at the machine, ready to unplug. Flash over USB first; don't rely on OTA.

- [ ] **Step 1: Stroke ratio matches duty (no portafilter, cup under the group, Brew switch on).**

Use this PowerShell sampler while a shot runs. It prints the fired-stroke ratio next to the commanded duty about every 2 s:
```powershell
$prev = $null
1..8 | ForEach-Object {
  $s = curl.exe -s http://gaggia.local/status | ConvertFrom-Json
  if ($prev) { "{0,-10} duty {1,5:N1}%  ratio {2:N3}" -f $s.phase_name, $s.pump_power, (($s.fired_cycles - $prev.fired_cycles) / (($s.zc_count - $prev.zc_count) / 2)) }
  $prev = $s; Start-Sleep -Milliseconds 2000
}
```
- **Flush (100%):** the ratio should be 1.00 ± 0.02.
- **Classic 9 bar, during "Fill":** the model holds about 37% duty while the estimate is in the headspace. The ratio should be within 0.03 of `pump_power / 100`.
- In both cases the strokes should sound even, with no stutter.

- [ ] **Step 2: Mains-loss watchdog.**

Start Flush, and flip the Brew switch off mid-run. Expect:
- `pump_mains` → `absent` within about 0.2 s;
- the Now banner "No mains to the pump…" appears;
- the gate stays low (the pump is silent).

Flip it back on: the pump resumes, ramped. Then stop the shot.

- [ ] **Step 3: Calibration.**

Remove the portafilter, put the tared cup on the scale, Brew switch on. In Settings → Pump calibration, do 3 runs and enter the grams each time:
- each run should read 0.15–0.40 ml/stroke;
- if the card flags a run as more than 10% off, redo it;
- **Save**: "Current value" shows the mean without "(default)", and the Now-tab calibration banner disappears.

Also check the abort path: start a run, and press **Stop** at about 3 s. The status should read "Run aborted: stopped", and that run stays "—".

- [ ] **Step 4: One shot per built-in, plus Flush.**

Use a dialed grind, about 18 g. For each of Classic 9 bar, Londinium, Lever, IUIUIU Classic and Blooming:
- the phases advance in the shown order;
- the shot ends on its own;
- the 5-bar sensor line rises during the low-pressure phases (a rough cross-check on est. bar);
- the History tab logs the shot.

Then run Flush: it must **not** appear in History.

- [ ] **Step 5: Trim direction.**

Run Classic 9 bar at trim −10, 0 and +10 (Tweak), with the same dose and grind. Shot yield in the same time should rise with trim, i.e. more duty means faster flow. Record the results in the AGENTS.md change log entry from Task 11, then commit:

```powershell
git add AGENTS.md
git commit -m "Docs: record live verification of phase profiles and pump calibration`n`nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

- [ ] **Step 6: Push and open a PR (ask the user first).**

```powershell
git push -u origin feature/phase-profiles-open-loop-pump
gh pr create --base main --title "Phase profiles, open-loop pump model, dimmer hardening, pump calibration" --body "Implements docs/superpowers/specs/2026-10-02-phase-profiles-open-loop-pump-design.md (plan: docs/superpowers/plans/2026-10-02-phase-profiles-open-loop-pump.md).`n`n🤖 Generated with [Claude Code](https://claude.com/claude-code)"
```
