# Phase-based shot profiles + open-loop pump model + dimmer hardening — Design

**Date:** 2026-10-02
**Status:** Approved in brainstorming (all 6 sections); awaiting written-spec review
**Branch:** `feature/phase-profiles-open-loop-pump` (from `origin/main` @ `081e068`, PR #23)

## 1. Goal and context

**What the user asked for:** research how Gaggiuino and GaggiMate use their pressure sensors and how much pump power they use to reach extraction pressure. Then add proper shot profiles to this app, and make the dimmer/pump control as good as it can be **without** a usable pressure sensor.

**Constraints (stated by the user or already in the repo):**
- The fitted transducer is a **0–0.5 MPa (0–5 bar)** part ("G41 0.5Mpa IN5V") on 3V3. It saturates within 0.3 s of pump start, even with an empty basket (AGENTS.md, 2026-10-01). It can't read brew pressure, and at the pump outlet it's probably being pushed past its ~2× overload rating on every shot.
- **User decision:** keep the 5-bar part for now and fit a 0–16 bar sensor later. The design must switch to closed-loop pressure control **without re-authoring profiles**.
- **User decision:** the pump % for each phase comes from a **model fitted by a guided scale calibration**. That calibration is **3 runs, averaged, with an outlier check**.
- **User decision:** built-in phase profiles plus simple tweaks; no full phase editor yet. The stored format must still support one later.
- **User decision:** harden the dimmer across the board. No specific symptom was reported.
- Shots end **by time**. There's no scale integration yet, so weight-based stops wait for roadmap item 5.

**Already in place, kept:**
- pulse-skip (PSM) dimmer with an integer-only ISR and Bresenham spreading (`src/dimmer.cpp`);
- `PRESSURE_CLOSED_LOOP_AVAILABLE false`;
- the pressure PID object;
- the `stateMutex` / `controlTick()` 50 ms control task;
- the safety cutoffs.

**Success criteria:**
1. Every built-in profile runs end-to-end on the real machine with the expected phase sequence and audibly even pump strokes.
2. After calibration, the 9-bar-target phases give a plausible shot (about 36 g in about 25–30 s with a dialed grind) at trim 0, and trim moves the result in the expected direction.
3. Changing `PRESSURE_CLOSED_LOOP_AVAILABLE` to `true` (after a 0–16 bar sensor is fitted and calibrated) needs **no profile changes**.
4. No regression in heater safety, the temperature PID, the shot log, or the pressure-ceiling cutoff.

## 2. Research summary (sources: Gaggiuino `release/stm32-blackpill`, GaggiMate `master`)

**Pressure sensors:**
- Both use 0–1.2 / 0–1.6 MPa, 0.5–4.5 V ratiometric sensors on 5 V.
- Both read them through an ADS1115.
- Gaggiuino samples every 10 ms; GaggiMate at 500 SPS, averaged per mains period.
- Both filter with a Kalman filter.

**Pump drive:** both use the PSM library. It does whole half-cycle pulse-skip with an accumulator, not phase-angle firing, and its divider auto-detects whether the zero-cross module reports one or both edges.

**Open-loop pump models:**
- **Gaggiuino:** `flowPerClick(P)` is a polynomial with coefficients `{0.045, 0.015, 0.0033, 0.000685, 0.000045, 0.009, -0.0018}`. The default `pumpFlowAtZero` is 0.2225 ml/click, and the user-calibrated range is 0.21–0.31. With `fpc_multiplier = 60/maxCPS`, that gives about 0.213 ml/click at 0 bar, 0.108 at 4 bar and 0.053 at 9 bar.
- **GaggiMate:** linear, 10.205 ml/s at 1 bar and 5.521 ml/s at 9 bar at full duty.
- The two models disagree by about 2× at 9 bar. That's why per-machine calibration matters.

**Duty to hold about 9 bar** with a puck passing 1.5–2 ml/s: about 55–75% (Gaggiuino model) or about 27–36% (GaggiMate model). For a pre-infusion fill at 2–3 bar and about 3 ml/s, it's about 31–40%. Neither project publishes a measured figure.

**Target pressure:** 9 bar is the usual peak, but neither project's defaults hold it flat. They use a short 8–9 bar peak followed by a decline or flow control (Londinium, Adaptive, lever styles); IUIUIU peaks at 7.5 bar.

**Profile schema:** both use phase lists. Each phase has a pressure or flow target, a transition curve (instant, linear or ease), a secondary limit, and OR-combined stop conditions.

## 3. Profiles and schema

### 3.1 Storage
- **New file:** `/profiles.json` on LittleFS.
- **Built-ins:** a constant JSON in firmware flash. They can't be deleted. The only changes allowed are *tweak overrides*, stored in `/profiles.json` under `builtin_overrides[name]`, and "Reset to default" removes the override.
- **User profiles:** up to `USER_PROFILE_MAX = 8`, created with Duplicate (from a built-in or another user profile).
- **Selection:** the active profile is stored in NVS by **name**, not by index, so reordering or deleting can't silently point it at another profile. If the name isn't found at boot, it falls back to "Classic 9 bar".

### 3.2 Profile JSON
```json
{ "name": "Londinium", "builtin": true, "temp": 92.0, "max_sec": 45,
  "pressure_scale": 1.0, "trim_pct": 0,
  "phases": [
    { "name": "Fill", "type": "flow", "target": 6.0, "limit": 4.0,
      "curve": "instant", "ramp_s": 0, "duration_s": 8,
      "stop": { "press_gte": 4.0 } } ] }
```
- **`type`:** `pressure` (bar), `flow` (ml/s), `power` (% duty, raw, used by utilities) or `soak` (pump off; target ignored).
- **`target`:** the phase's end value.
- **`limit`:** optional secondary bound. On a `pressure` phase it's a max flow in ml/s; on a `flow` phase, a max bar. 0 or absent means none.
- **`curve` + `ramp_s`:** the target moves from the previous phase's end value to `target` over `ramp_s` using `instant`, `linear`, `ease-in`, `ease-out` or `ease-in-out`, then holds. For the first phase the start value is 0. A ramp after a `soak` starts from the model's current `P_est` (pressure phases) or 0 ml/s (flow phases).
- **`duration_s`:** the phase's hard time cap. It's always present, and it is the **only** exit while the sensor isn't usable.
- **`stop`:** optional `press_gte` and `press_lte`, OR-combined with the duration. **Only evaluated when `PRESSURE_CLOSED_LOOP_AVAILABLE` is true** and the pressure sensor isn't faulted.
- **Tweak fields (allowed on built-ins):** `temp`, `max_sec`, `pressure_scale` (0.7–1.1; multiplies `pressure` targets, pressure `limit`s and `press_*` stops), and `trim_pct` (−20…+20).
- **Validation on load and save:**
  - 1–8 phases (utility profiles: up to 10);
  - a known `type` and `curve`;
  - `duration_s` 1–120 and `ramp_s` 0 up to `duration_s`;
  - pressure targets 0–11 bar;
  - flow targets 0–10 ml/s;
  - power 0–100%;
  - `temp` within the existing brew-target clamp;
  - `max_sec` 5–90 (the existing `SHOT_AUTO_STOP_SEC_MIN/MAX`).

  An invalid profile is skipped with a Serial log line and never crashes the boot.

### 3.3 Built-in profiles
Values are model estimates; every phase exits on its duration without a sensor.

| Profile | Temp | max_sec | Phases |
|---|---|---|---|
| Classic 9 bar | 93 | 35 | Fill: flow 4 ml/s, 4 bar limit, 6 s → Ramp: pressure 9, linear 3 s, 3 s → Hold: pressure 9, 26 s |
| Londinium | 92 | 45 | Fill: flow 6, 4 bar limit, 8 s → Soak: 10 s → Rise: pressure 9, ease-out 2 s, 2 s → Hold: pressure 9, 4 s → Decline: pressure 3, linear 20 s, 20 s |
| Lever (Cremina-style) | 88 | 50 | Pre-infuse: pressure 1.5, 8 s → Hold: pressure 1.5, 7 s → Rise: pressure 9, ease-in-out 8 s, 8 s → Decline: pressure 3, linear 25 s, 25 s |
| IUIUIU Classic | 93 | 45 | PI: flow 3, 4 bar limit, 10 s → Soak: 8 s → Rise: pressure 7.5, ease-out 5 s, 5 s → Ease: pressure 6, ease-in-out 4 s, 4 s → Flow: flow 1.0 (ramps from 2.5), ease-in-out 15 s, 6 bar limit, 15 s |
| Blooming | 93 | 55 | Fill: flow 4, 4 bar limit, 8 s → Bloom: soak 25 s → Ramp: flow 2, ease-out 5 s, 5 s → Flow: flow 2, 6 bar limit, 15 s |
| Flush (utility) | – | 10 | Power 100%, 5 s |
| Backflush (utility) | – | 100 | 5 × [Power 100%, 10 s → Soak 10 s] (10 phases: the only built-in above the 8-phase limit, allowed for utilities) |

Utility profiles skip the temperature setpoint and are excluded from the shot-history log.

In IUIUIU's last phase, the ramp starts at 2.5 ml/s explicitly, using an optional per-phase `start` field. That field is allowed on built-ins only for now and isn't exposed in the UI.

### 3.4 Migration
- **Source:** the existing file at `/profiles.csv`. Despite the extension, it has been a flat JSON array since 2026-08-23, holding the user's 4 profiles (Classic, Medium Roast, Light Roast, Dark Roast).
- **Order of operations:** if `/profiles.json` doesn't exist and `/profiles.csv` does, the old file is first copied **byte-for-byte to `/profiles.csv.bak`**, and only then converted.
- **Mapping:**
  - pre-infusion pulses become alternating `power` (at the old global `pumpPreinfusionPowerPct`) and `soak` phases, using `on_ms` and `off_ms`;
  - the pressure ramp becomes `pressure` at `pressure_ramp_bar` for `pressure_ramp_ms`;
  - the optional decline becomes `pressure` at `pressure_decline_bar`;
  - then a final `pressure` 9-bar phase, or `power` at the old `pumpBrewPowerPct` if the old profile had pressure disabled, until `auto_stop_sec`;
  - `temp` and `max_sec = auto_stop_sec` carry over, and `trim_pct = 0`.
- **Overflow:** if the pulses would need more than 8 phases, the pulse count is reduced and a Serial warning is logged.
- **Selection:** the active profile becomes the migrated profile with the same name as before.
- **NVS clean-up:** the old NVS keys (`pi_*`, `press_*` and the global pump-power keys) are left in place but unused, and are listed in the change log for later removal.

## 4. Pump model and shot planner

### 4.1 `pump_model.h/.cpp` (pure C++, no Arduino includes, unit-tested on the PC)

**Pump side:**
- `fpcShape(P)` is Gaggiuino's polynomial, normalized so `fpcShape(0) = 1`.
- `pumpMlPerClick(P) = calMlPerClick × fpcShape(P)`, with `calMlPerClick` from calibration (default 0.2225).
- `pumpFlow(P, duty) = duty × cps × pumpMlPerClick(P)`, where `cps` is the measured mains frequency (one stroke per cycle).
- For P ≥ 11 bar, `pumpMlPerClick` is clamped to a small positive floor so the duty maths never divides by 0 or goes negative.

**Puck side:** `puckFlow(P) = Qref × P / 9`, with `Qref = 2.0 ml/s × (1 + trim_pct/100)`. This is the reference puck: a dialed 18 g → 36 g shot at 9 bar, passing about 2 ml/s. It's linear, in the Darcy sense.

**State:**
- Estimated pressure is integrated every tick: `P_est += dt × (pumpFlow(P_est, duty) − puckFlow(P_est)) / C`, with `C = 1.4 ml/bar`, then clamped to 0–16.
- Estimated volume pumped: `ml_est += dt × pumpFlow(P_est, duty)`.
- Both reset to 0 at shot start.

**Duty laws** (P is `P_est` while the sensor is unusable, otherwise the measured pressure):
- **pressure target T:** `duty = (puckFlow(P) + C × (T − P)/τ) / (cps × pumpMlPerClick(P))`, with `τ = 0.5 s`.
- **flow target F:** `duty = F / (cps × pumpMlPerClick(min(P, limitBar or P)))`.
- **limit on a pressure phase (max flow L):** `duty = min(duty, L / (cps × pumpMlPerClick(P)))`.
- **limit on a flow phase (max bar B):** `duty = min(duty, pressureDuty(B))`.
- **power:** `duty = target/100`. **soak:** `duty = 0`.
- All duties are clamped to 0–1.

**Closed loop (future, when `PRESSURE_CLOSED_LOOP_AVAILABLE`):** the same laws are fed the measured pressure. The existing `pressurePID` adds a ± correction around the model duty, so the model acts as feedforward and the PID trims it. The existing PID object and gains are reused, and this path is compiled but not active.

### 4.2 `shot_planner.h/.cpp`
- `plannerStart(const Profile&)` resets the phase index and the model state, and records each phase's start value.
- `plannerTick(nowMs, measuredPressure, sensorUsable)` returns `{dutyPct, phaseIndex, phaseName, target, P_est, ml_est, done}`. It runs every 50 ms from `controlTick()` under the existing lock.
  - **Target at time t in a phase:** `start + (end − start) × curve(min(t/ramp_s, 1))`. Ease functions are cubic: in = t³, out = 1−(1−t)³, in-out = the standard piecewise cubic.
  - **Advance** when `t ≥ duration_s`, or when a `stop` condition holds (only if the sensor is usable).
  - **Done** after the last phase or at `max_sec`.
- **Integration with `main.cpp`:**
  - `startShot()` / `stopShot()` call the planner instead of `buildShotStages()`.
  - The `ShotStage` struct, `activeShotStages`, `pumpBrewPowerPct`, `pumpPreinfusionPowerPct` and the `preinfusionFill*` / `preinfusionSoak*` globals are removed.
  - `currentShotPhase` is still reported, derived from the phase type, for compatibility with the shot log.
  - The existing `shotAutoStopSec` setting is replaced by the profile's own `max_sec`.
- **Safety is unchanged.** The forced-0% cutoffs in `controlTick()` (pressure fault while the closed loop is active, `currentPressure > PUMP_MAX_SAFETY_BAR`, any shot stop) still override the planner. They run after it in the same tick.

## 5. Dimmer hardening (`dimmer.cpp`)

1. **Long gate pulse across the crossing.**
   - The gate goes high **inside the zero-cross ISR** on fired cycles and is held for `DIMMER_GATE_HOLD_US = 1500`.
   - It's dropped by a **gptimer one-shot ISR**, which replaces the `esp_timer` task callback with its 100 µs busy-wait and the 300 µs firing delay.
   - Why: the pulse spans the true crossing whatever the module's zero-cross offset, and it gives the inductive pump current time to rise past the TRIAC's latching current. That eliminates lost strokes.
2. **Mains frequency auto-detect.**
   - A rolling 1 s count of accepted crossings sets `cps` (crossings ÷ 2) and the debounce window (half of the measured half-cycle).
   - It assumes 50 Hz until the first full window.
   - Below 45 or above 65 Hz is reported as `unstable`.
3. **Zero-cross watchdog.**
   - `dimmerGetMainsState()` returns `OK`, `ABSENT` (no crossing for more than 100 ms) or `UNSTABLE`.
   - While the state is ABSENT, nothing fires and the accumulator is reset.
   - `/status` reports `pump_mains`.
4. **Permanent counters:** `dimmerGetZcCount()` (kept, no longer "temporary") and `dimmerGetFiredCycles()`. Both are 32-bit and ISR-incremented.
5. **Slew limit.**
   - It's applied in task context: `dimmerSetPowerPercent()` gets a ramp target, and a small `dimmerTick(dtMs)` called from `controlTick()` steps the actual value up by at most `DIMMER_SLEW_PCT_PER_S = 250`.
   - **Decreases are instant**, and 0% stays instant, including cancelling a pending gate.
   - `dimmerSetPowerPercentImmediate()` skips the slew; it's used only by calibration.
6. **Clean start.** When power goes up from 0, the Bresenham accumulator is set to 500 (half of 1000), so the first stroke comes promptly and the spacing is even.

The ISR stays integer-only, with no float, as the existing FPU-safety note requires.

## 6. Pump calibration

**Steps:**
1. Settings → Brewing → "Pump calibration". The user removes the portafilter, puts a tared cup on the scale and turns the Brew switch on. The **Run** button is disabled unless `pump_mains == ok`.
2. A run drives the pump at **100% via `dimmerSetPowerPercentImmediate`** (no slew) for exactly 10.0 s. It records `fired_cycles` at the start and the end, then stops.
3. The user enters grams. That run's value is `ml/click = grams / Δfired_cycles`, with 1 g = 1 ml.
   - **Plausibility:** each value must be 0.15–0.40, otherwise "check the entry".
4. Three runs are required. If any run is more than 10% from the 3-run mean, it's flagged and the user is offered `pump_cal_redo=<n>`.
5. **Save** stores `pump_ml_per_click` (the mean), the three run values and the date in NVS. These are also included in `/settings_export` and restored by import.

**Rules:**
- A run can't start while a shot, autotune or another calibration run is active.
- Starting a shot cancels any in-progress calibration run.
- **Abort and safety:** if any safety path cuts the pump during a run (pressure ceiling, fault, mains `ABSENT`, the Stop button, a shot start), the run is **discarded** and reported as "aborted: <reason>". It is never stored as a short measurement.
- **The 12-bar ceiling doesn't affect a run.** With the 5-bar sensor, `currentPressure` is clamped at ≤5 and implausible readings are dropped, so `currentPressure > 12` can't trigger. Free flow without a portafilter is about 0 bar anyway. The ceiling stays armed for the future 0–16 bar sensor.

**API and status:**
- `/update?pump_cal=run|save|cancel`, `pump_cal_grams=<g>`, `pump_cal_redo=<n>`.
- `/status` gains `pump_cal_state` (`idle`, `running`, `await_grams` or `aborted`, plus the reason), `pump_cal_runs[]` and `pump_ml_per_click`.

## 7. Web UI (`web.cpp`, existing 2026-09-15 visual language)

- **Now tab:**
  - The pressure ring shows **est. bar** (`P_est`) with an "estimated" tag. The raw sensor reading is shown as "sensor (0–5 bar)".
  - During a shot it shows the phase name and index, the target, duty and ml (est.).
  - Banners: "Pump not calibrated" and "No mains to pump, flip the Brew switch".
- **Profiles tab:**
  - A list of built-in and user profiles, each with a mini inline-SVG phase chart of target over time.
  - Tap to select.
  - Built-ins have Tweak, Duplicate and Reset. User profiles have Tweak, Rename, Duplicate and Delete (with the existing arm-then-confirm). Tweak covers temp, max_sec, pressure_scale and trim.
  - The old pulse/ramp/decline editor is removed.
- **Shot chart:** the dashed target curve is overlaid on est. bar and duty, and the phase markers are kept.
- **Settings → Brewing:**
  - the calibration card is added;
  - the global brew-power and pre-infusion-power fields and the old fill/soak settings are removed;
  - the shot auto-stop setting is removed, because it's now per profile (`max_sec`).
- **Firmware:** pressure-PID tuning stays in a collapsed `<details>`, labelled "used once a 0–16 bar sensor is fitted".
- **Endpoints:**
  - `/profiles` (GET) returns built-ins with overrides applied, plus user profiles.
  - `/update` takes `profile_select=<name>`, `profile_tweak=<name>&temp=…&max_sec=…&pressure_scale=…&trim_pct=…`, `profile_duplicate=<name>&new_name=…`, `profile_rename=<old>&new_name=…`, `profile_delete=<name>` and `profile_reset=<name>`.
  - User-supplied names are rendered with `textContent` (the existing XSS rule).
- `/status` gains `phase_name`, `phase_index`, `phase_count`, `phase_target`, `pressure_est`, `ml_est`, `pump_mains` and `pump_duty`.

## 8. Testing

**Unit tests:**
- **Setup:** add `[env:native]` (platform `native`, Unity) compiling `pump_model.cpp`, `shot_planner.cpp` and the profile parse/validate/migrate code. To make that possible, profile JSON handling is split into `profile_codec.cpp`: ArduinoJson only, no LittleFS. If no host compiler is available, the same suite runs on the board with `pio test -e esp32-s3-devkitc-1`.
- **pump_model:**
  - `fpcShape` matches Gaggiuino at 0, 4 and 9 bar (within 2%);
  - a simulated 9-bar step settles with ≤0.5 bar overshoot within 5 s;
  - a declining target lowers duty;
  - duty is always within [0, 1];
  - at P ≥ 11 there's no division by zero.
- **Curves:** endpoints are exact; every curve is monotonic.
- **shot_planner:**
  - phase order and durations;
  - `ramp_s` interpolation;
  - `pressure_scale` and trim applied;
  - `stop` ignored when `sensorUsable == false` and honored when true;
  - `max_sec` ends the shot.
- **profile_codec:**
  - each built-in validates;
  - the rejection cases (more than 8 phases on a non-utility profile, unknown type, out-of-range values);
  - migration of a fixture holding the device's 4 real profiles, including the pulses → phases mapping.
- **Calibration maths:** the mean, the 10% outlier detection and the plausibility bounds.

**Bench (USB only, mains off):**
- build and flash;
- `/status` has the new fields;
- `/profiles` lists 7 built-ins plus the 4 migrated profiles;
- `/profiles.csv.bak` exists;
- select, tweak, duplicate and delete all work.

**Supervised live test** (user at the machine, ready to unplug):
1. Fixed power with no portafilter at 100%, 50% and 25%: `Δfired/Δ(zc/2)` matches the duty within 2%, and the strokes sound even.
2. Flip the Brew switch off mid-run: `pump_mains` goes `absent`, the gate stays low, and nothing fires when power returns until a new command arrives.
3. The 3-run calibration, then save.
4. One shot on each built-in plus Flush, watching the 5-bar sensor during the low-pressure phases as a cross-check on `P_est`.
5. Classic 9 bar at trim −10, 0 and +10: shot time and yield move in the expected direction.

Flash over USB (OTA has been unreliable).

## 9. Out of scope (YAGNI)
- A full phase editor UI. The format supports it later.
- Weight- or volume-based stops (waits for the BLE scale, roadmap item 5).
- Sensor hardware changes. The 0–16 bar sensor swap and its calibration are a separate task. This design only guarantees it's a one-flag change plus PID tuning.
- Kalman filtering of the pressure sensor (only useful with the real-range sensor).

## 10. Docs
- **AGENTS.md:**
  - Section 4: the new modules, the profile system, the pump model and calibration.
  - Remove the "temporary zero-cross diagnostic" notes.
  - Change-log entry, including the list of now-unused NVS keys.
- **HARDWARE_ROADMAP.md item 8:** the dimmer gate-hold change and the calibration procedure.
- **`config.h`:** the comments about the old pre-infusion fill modes are replaced.
