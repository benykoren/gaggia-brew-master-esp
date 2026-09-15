# Unified BLE/Serial Connectivity Protocol Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the ESP32-S3 firmware a transport-agnostic command/telemetry
protocol that BLE (NimBLE) and USB Serial both speak, with full feature
parity with the existing Web UI (except OTA, which stays WiFi-only), by
extracting `web.cpp`'s `/update`/`/status` logic into a shared,
transport-agnostic dispatch layer and adding BLE + Serial transports on top
of it.

**Architecture:** A new `command_dispatch.cpp` owns the one real refactor:
`handleUpdate()`'s ~60-field validation/clamping/persistence logic moves
into `applyUpdateParams(const ParamSource&)`, and `handleStatus()`'s
JSON-building moves into `buildStatusJson(bool includeHistory)`; `web.cpp`
keeps its HTTP-specific bits (routing, redirect, OTA) as thin wrappers
around both. A new `connectivity.cpp` runs its own FreeRTOS task (pinned to
core 0, priority between the Arduino loop task and `controlLoopTask`),
polling a new `serial_transport.cpp` (line-buffered JSON over the existing
USB-CDC Serial link) and a new `ble_transport.cpp` (NimBLE GATT service:
Notify telemetry, Write command, Notify response), dispatching JSON
commands through the same `command_dispatch` functions the Web UI uses, and
pushing a compact 20-byte binary telemetry frame (`telemetry.cpp`) to both
transports on a timer. `controlLoopTask` and its `stateMutex` are untouched.

**Tech Stack:** ESP32-S3 / Arduino framework / PlatformIO, `bblanchon/ArduinoJson`
(existing dependency, already v7 - no new JSON library needed), new
dependency `h2zero/NimBLE-Arduino` (chosen over the built-in Bluedroid BLE
stack for WiFi coexistence and lower RAM/flash footprint - spec §3).

**Spec:** `docs/superpowers/specs/2026-09-04-connectivity-protocol-design.md`

## Global Constraints

- Build command: `& "C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\pio.exe" run -e esp32-s3-devkitc-1`
- No unit-test framework exists in this project (embedded, Arduino-tied, no
  `test/` dir) - every code task's verification step is a clean `pio run`
  build; real behavior is verified in the two bench-milestone tasks at the
  end (Tasks 9-10), matching how every prior feature in this codebase was
  verified.
- `controlLoopTask` (temperature + pump-pressure PID, pinned to core 1,
  `configMAX_PRIORITIES - 2`) and its `CONTROL_TASK_PERIOD_MS` cadence are
  **not modified by this plan** - it already runs on its own task (this was
  built during the pump-pressure/watchdog work, ahead of this spec). Nothing
  here changes its priority, core, or timing.
- The existing recursive `stateMutex` (`lockState()`/`unlockState()`,
  declared in `main.cpp`) is reused as-is for every new code path that reads
  or writes control-loop-owned globals - no new locking primitive.
- BLE library: NimBLE-Arduino, not the ESP32 core's built-in Bluedroid stack
  (spec §3).
- New connectivity task: pinned to **core 0** (alongside WiFi/AsyncTCP),
  priority **above** the Arduino loop task's default priority (1) and
  **below** `controlLoopTask`'s (spec §6).
- JSON `"params"` field names in the `"update"` command must be **exactly**
  the same names `/update`'s query args already use (spec §4b) - no new
  vocabulary.
- OTA stays WiFi-only - this protocol adds no OTA commands (spec §9).
- WiFi and BLE run simultaneously; this plan never disables WiFi.
- Every safety-relevant clamp (`steam_max_safety`, pressure-profile bar
  clamps, `shot_auto_stop_sec`, etc.) must apply identically regardless of
  which transport a command arrived over (spec §7) - achieved by definition
  here, since all three transports funnel through the same
  `applyUpdateParams()`.

---

## Task 1: NimBLE dependency + connectivity constants

**Files:**
- Modify: `platformio.ini:25-31` (`lib_deps` block, `esp32-s3-devkitc-1` env)
- Modify: `include/config.h` (append a new block at the end, after line 473)

**Interfaces:**
- Produces: `CONNECTIVITY_TASK_STACK_SIZE`, `CONNECTIVITY_TASK_PRIORITY`,
  `CONNECTIVITY_TASK_POLL_MS`, `CONNECTIVITY_TELEMETRY_INTERVAL_MS`,
  `TELEMETRY_FRAME_LEN`, `TELEMETRY_SYNC_MARKER`, `SERIAL_MAX_LINE_LEN`,
  `BLE_MAX_COMMAND_LEN`,
  `BLE_DEVICE_NAME`, `BLE_SERVICE_UUID`, `BLE_CHAR_TELEMETRY_UUID`,
  `BLE_CHAR_COMMAND_UUID`, `BLE_CHAR_RESPONSE_UUID` - all consumed by later
  tasks.

- [ ] **Step 1: Add the NimBLE-Arduino dependency**

In `platformio.ini`, inside the `esp32-s3-devkitc-1` env's `lib_deps` block:

```ini
lib_deps =
    br3ttb/PID @ ^1.2.1
    tzapu/WiFiManager @ ^2.0.17
    knolleary/PubSubClient @ ^2.8
    esp32async/AsyncTCP @ ^3.5.0
    esp32async/ESPAsyncWebServer @ ^3.12.0
    bblanchon/ArduinoJson @ ^7.2.0
    h2zero/NimBLE-Arduino @ ^1.4.3
```

- [ ] **Step 2: Add connectivity constants to `config.h`**

Append to the end of `include/config.h` (after the existing scheduled-warmup
block):

```cpp
// ============================================================================
// Unified BLE/Serial connectivity protocol (2026-09-15, spec:
// docs/superpowers/specs/2026-09-04-connectivity-protocol-design.md)
// ----------------------------------------------------------------------------
// A dedicated FreeRTOS task (connectivity.cpp) polls both transports and
// dispatches JSON commands through the same command_dispatch.cpp functions
// the Web UI's /update and /status use, and pushes a compact binary
// telemetry frame to both transports on a timer. Pinned to core 0
// (alongside WiFi/AsyncTCP/NimBLE's own host task) - controlLoopTask's core
// 1 is left completely undisturbed (spec §6).
// ============================================================================
#define CONNECTIVITY_TASK_STACK_SIZE 8192
// Above the Arduino loop task's default priority (1), well below
// controlLoopTask's (configMAX_PRIORITIES - 2) - spec §6. Any value in
// between satisfies the requirement; 2 is the smallest such value.
#define CONNECTIVITY_TASK_PRIORITY 2
#define CONNECTIVITY_TASK_POLL_MS 20
// Matches the existing Web UI's /status poll cadence (spec §4a).
#define CONNECTIVITY_TELEMETRY_INTERVAL_MS 2000

// Binary telemetry frame (spec §4a) - fixed 20 bytes, little-endian. This
// is exactly BLE's guaranteed-available payload at the default,
// un-negotiated 23-byte ATT MTU (MTU - 3 bytes of ATT overhead) - matters
// because BluetoothGatt.requestMtu() doesn't exist before Android API 21,
// so any pre-Lollipop client (KitKat's API 19 included) can never
// negotiate more than this, ever. The frame's sync marker doubles as its
// protocol version (no separate version byte) specifically to fit this
// budget - see spec §4a's 2026-09-15 correction.
#define TELEMETRY_FRAME_LEN 20
// Distinguishes a binary telemetry frame from a JSON command line (which
// always starts with '{' / 0x7B) on the shared Serial link, and doubles as
// the protocol version (a future incompatible frame layout uses a
// different marker byte rather than growing the frame).
#define TELEMETRY_SYNC_MARKER 0xA5

// Longest JSON command line this firmware will buffer before giving up on
// it (Serial) or accept in one BLE write (BLE) - generous for the largest
// real command ("update" with several profile_press_* fields at once), and
// bounded so a stray/binary byte stream on either transport can't grow a
// buffer unboundedly.
#define SERIAL_MAX_LINE_LEN 512
#define BLE_MAX_COMMAND_LEN 256

// BLE GATT layout (spec §5) - one custom primary service, three
// characteristics. UUIDs are random v4-style, generated once here and then
// fixed for the life of this protocol (a client hardcodes them to discover
// the service).
#define BLE_DEVICE_NAME "GaggiaBrewMaster"
#define BLE_SERVICE_UUID        "7a2f9c00-6b3e-4f1a-9d3b-2a0e4f6b8c10"
#define BLE_CHAR_TELEMETRY_UUID "7a2f9c01-6b3e-4f1a-9d3b-2a0e4f6b8c10"
#define BLE_CHAR_COMMAND_UUID   "7a2f9c02-6b3e-4f1a-9d3b-2a0e4f6b8c10"
#define BLE_CHAR_RESPONSE_UUID  "7a2f9c03-6b3e-4f1a-9d3b-2a0e4f6b8c10"
```

- [ ] **Step 3: Build**

Run: `& "C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\pio.exe" run -e esp32-s3-devkitc-1`
Expected: PASS (first run downloads NimBLE-Arduino - several minutes). No
functional change yet - these constants aren't referenced by any code until
later tasks.

- [ ] **Step 4: Commit**

```bash
git add platformio.ini include/config.h
git commit -m "Add NimBLE dependency and connectivity protocol constants"
```

---

## Task 2: Shared command-dispatch layer - `applyUpdateParams()`

**Files:**
- Create: `include/command_dispatch.h`
- Create: `src/command_dispatch.cpp`
- Modify: `src/web.cpp:1846-2150` (`handleUpdate()` becomes a thin wrapper)

**Interfaces:**
- Produces: `class ParamSource` (pure virtual `has()`/`get()`), `bool
  applyUpdateParams(const ParamSource &params)` (returns true if the caller
  should restart after responding - `mqtt_server` was among the changed
  fields), `bool isKnownUpdateField(const String &name)`.
- Consumes: every global `web.cpp:17-87` already declares `extern` (moved
  here verbatim - see Step 1), plus `profiles.h`/`shot_log.h`'s existing
  function declarations.

- [ ] **Step 1: Create `include/command_dispatch.h`**

```cpp
#pragma once

#include <Arduino.h>

// A read-only, named-field view over a command's parameters. Lets
// applyUpdateParams() below stay transport-agnostic: web.cpp adapts an
// AsyncWebServerRequest's query args, connectivity.cpp adapts a parsed JSON
// "params" object - same interface either way.
class ParamSource {
 public:
  virtual bool has(const char *name) const = 0;
  virtual String get(const char *name) const = 0;
  virtual ~ParamSource() {}
};

// Applies every field /update already accepts (mode, tuning, shot
// start/stop, profiles, schedules, MQTT config, etc.), sourced from
// `params` instead of a specific transport - same validation, clamping,
// persistence (NVS), and side effects (setOpMode(), startShot(),
// profileSave(), etc.) regardless of which transport called it. Holds
// main.cpp's stateMutex for its own full body (see config.h "Shared-state
// lock") - callers don't need to lock around this themselves.
// Returns true if the caller should restart the board after sending its
// own response (mqtt_server was among the changed fields) - matches the
// existing HTTP behavior (handleUpdate() used to call ESP.restart() itself
// after redirecting; that decision now belongs to each transport's caller,
// since only HTTP can "redirect").
bool applyUpdateParams(const ParamSource &params);

// True if `name` is one of the field names applyUpdateParams() recognizes.
// HTTP keeps silently ignoring unknown query args (unchanged, matches
// browser form behavior); the BLE/Serial JSON command layer uses this to
// reject an unknown field as a protocol-mismatch error instead (spec §8).
bool isKnownUpdateField(const String &name);

// Builds the same JSON object /status returns. `includeHistory=false` omits
// the "history"/"pressure_history" arrays - used by the {"cmd":"status"}
// BLE/Serial command, which relies on the higher-rate binary telemetry
// stream instead of polling this (spec §4b).
String buildStatusJson(bool includeHistory);
```

- [ ] **Step 2: Create `src/command_dispatch.cpp`**

This moves `handleUpdate()`'s body out of `web.cpp` essentially unchanged
(only `hasArg("x")` → `params.has("x")` and `arg("x")` → `params.get("x")`),
including its own copy of the `extern` declarations `web.cpp:17-87` already
uses - `web.cpp` still needs its own copy too (for `/settings_export`,
`handleStatus()`, etc.), matching this codebase's existing pattern of
declaring the externs it needs at the top of each `.cpp` file rather than a
shared header.

```cpp
#include "command_dispatch.h"

#include <Preferences.h>
#include <PID_v1.h>

#include "config.h"
#include "dimmer.h"
#include "profiles.h"
#include "shot_log.h"

extern float currentTemperature;
extern bool sensorFault;
extern double Setpoint, Input, Output;
extern PID myPID;
extern PID pressurePID;
extern float tempHistory[];
extern int tempHistoryHead;
extern int tempHistoryCount;

extern float currentPressure;
extern bool pressureFault;
extern bool activePressureEnabled;
extern double activePressureRampBar;
extern unsigned long activePressureRampMs;
extern bool activePressureDeclineEnabled;
extern double activePressureDeclineBar;
extern unsigned long activePressureDeclineMs;
extern double pressureKp, pressureKi, pressureKd;
extern float pressureHistory[];
extern int pressureHistoryHead;
extern int pressureHistoryCount;

extern OpMode currentMode;
extern double brewSetpoint, brewKp, brewKi, brewKd;
extern double brewActiveKp, brewActiveKi, brewActiveKd;
extern double steamSetpoint, steamKp, steamKi, steamKd;
extern double steamMaxSafety;
extern void setOpMode(OpMode mode);
extern void refreshActiveProfileIfChanged();

extern unsigned long ecoTimeoutMin;
extern unsigned long steamAutoOffMin;
extern bool autoSleeping;
extern OpMode modeBeforeSleep;
extern void noteActivity();
extern void wakeFromSleep();

extern AutotuneState autotuneState;
extern String autotuneMessage;
extern void startAutotune(OpMode forMode);
extern void stopAutotune();

extern bool shotInProgress;
extern unsigned long shotStartMillis;
extern unsigned long shotAutoStopSec;
extern void startShot();
extern void stopShot();
extern ShotPhase currentShotPhase;

extern unsigned long shotCount;
extern time_t lastDescaleTime;
extern unsigned long descaleShotThreshold;
extern unsigned long descaleDayThreshold;
extern void markDescaled();

extern int activeProfileIndex;
extern bool activePreinfusionEnabled;
extern int activePreinfusionPulses, activePreinfusionOnMs, activePreinfusionOffMs;
extern void applyProfile(int idx);

extern bool schedEnabled[SCHED_MAX_COUNT];
extern int schedHour[SCHED_MAX_COUNT], schedMin[SCHED_MAX_COUNT];
extern bool schedModeSteam[SCHED_MAX_COUNT];
extern int schedTzOffsetMin;
extern void resetSchedFired(int i);

extern void lockState();
extern void unlockState();

bool applyUpdateParams(const ParamSource &params) {
  noteActivity(); // any update call is explicit user action - resets eco-sleep timer

  // Holds the whole body under stateMutex (see config.h "Shared-state
  // lock") - every field here is also read/written by controlTick() on its
  // own task.
  lockState();

  Preferences preferences;
  preferences.begin("gaggia", false); // false = read/write

  if (params.has("brew_target")) {
    brewSetpoint = params.get("brew_target").toDouble();
    preferences.putDouble("brew_target", brewSetpoint);
  }
  if (params.has("brew_kp")) {
    brewKp = params.get("brew_kp").toDouble();
    preferences.putDouble("brew_kp", brewKp);
  }
  if (params.has("brew_ki")) {
    brewKi = params.get("brew_ki").toDouble();
    preferences.putDouble("brew_ki", brewKi);
  }
  if (params.has("brew_kd")) {
    brewKd = params.get("brew_kd").toDouble();
    preferences.putDouble("brew_kd", brewKd);
  }
  if (params.has("brew_akp")) {
    brewActiveKp = params.get("brew_akp").toDouble();
    preferences.putDouble("brew_akp", brewActiveKp);
  }
  if (params.has("brew_aki")) {
    brewActiveKi = params.get("brew_aki").toDouble();
    preferences.putDouble("brew_aki", brewActiveKi);
  }
  if (params.has("brew_akd")) {
    brewActiveKd = params.get("brew_akd").toDouble();
    preferences.putDouble("brew_akd", brewActiveKd);
  }
  if (params.has("steam_target")) {
    steamSetpoint = params.get("steam_target").toDouble();
    preferences.putDouble("steam_target", steamSetpoint);
  }
  if (params.has("steam_kp")) {
    steamKp = params.get("steam_kp").toDouble();
    preferences.putDouble("steam_kp", steamKp);
  }
  if (params.has("steam_ki")) {
    steamKi = params.get("steam_ki").toDouble();
    preferences.putDouble("steam_ki", steamKi);
  }
  if (params.has("steam_kd")) {
    steamKd = params.get("steam_kd").toDouble();
    preferences.putDouble("steam_kd", steamKd);
  }
  if (params.has("press_kp")) {
    pressureKp = params.get("press_kp").toDouble();
    preferences.putDouble("press_kp", pressureKp);
  }
  if (params.has("press_ki")) {
    pressureKi = params.get("press_ki").toDouble();
    preferences.putDouble("press_ki", pressureKi);
  }
  if (params.has("press_kd")) {
    pressureKd = params.get("press_kd").toDouble();
    preferences.putDouble("press_kd", pressureKd);
  }
  if (params.has("press_kp") || params.has("press_ki") || params.has("press_kd")) {
    pressurePID.SetTunings(pressureKp, pressureKi, pressureKd);
  }
  if (params.has("steam_max_safety")) {
    double v = params.get("steam_max_safety").toDouble();
    steamMaxSafety = constrain(v, STEAM_MAX_SAFETY_MIN, STEAM_MAX_SAFETY_MAX);
    preferences.putDouble("steam_max_safety", steamMaxSafety);
  }
  if (params.has("brew_target") || params.has("brew_kp") ||
      params.has("brew_ki") || params.has("brew_kd") ||
      params.has("brew_akp") || params.has("brew_aki") ||
      params.has("brew_akd") || params.has("steam_target") ||
      params.has("steam_kp") || params.has("steam_ki") ||
      params.has("steam_kd") || params.has("steam_max_safety")) {
    refreshActiveProfileIfChanged();
  }

  if (params.has("mode")) {
    stopAutotune();
    String mode = params.get("mode");
    if (mode == "off") {
      setOpMode(OpMode::OFF);
    } else if (mode == "brew") {
      setOpMode(OpMode::BREW);
    } else if (mode == "steam") {
      setOpMode(OpMode::STEAM);
    }
  }

  if (params.has("shot_auto_stop_sec")) {
    long v = params.get("shot_auto_stop_sec").toInt();
    shotAutoStopSec = (v <= 0) ? 0 : constrain(v, SHOT_AUTO_STOP_SEC_MIN, SHOT_AUTO_STOP_SEC_MAX);
    preferences.putULong("shot_auto_stop", shotAutoStopSec);
  }

  if (params.has("eco_timeout_min")) {
    ecoTimeoutMin = params.get("eco_timeout_min").toInt();
    preferences.putULong("eco_min", ecoTimeoutMin);
  }
  if (params.has("steam_auto_off_min")) {
    steamAutoOffMin = params.get("steam_auto_off_min").toInt();
    preferences.putULong("steam_off_min", steamAutoOffMin);
  }
  if (params.has("wake") && params.get("wake") == "1") {
    wakeFromSleep();
  }

  if (params.has("autotune")) {
    String at = params.get("autotune");
    if (at == "start") {
      startAutotune(currentMode == OpMode::STEAM ? OpMode::STEAM : OpMode::BREW);
    } else if (at == "stop") {
      stopAutotune();
    }
  }

  if (params.has("shot")) {
    String s = params.get("shot");
    if (s == "start") {
      startShot();
    } else if (s == "stop") {
      stopShot();
    }
  }

  if (params.has("mark_descaled") && params.get("mark_descaled") == "1") {
    markDescaled();
  }
  if (params.has("descale_shot_threshold")) {
    descaleShotThreshold = params.get("descale_shot_threshold").toInt();
    preferences.putULong("descale_shots", descaleShotThreshold);
  }
  if (params.has("descale_day_threshold")) {
    descaleDayThreshold = params.get("descale_day_threshold").toInt();
    preferences.putULong("descale_days", descaleDayThreshold);
  }

  if (params.has("shot_note_index")) {
    int idx = params.get("shot_note_index").toInt();
    String bean = params.has("shot_note_bean") ? params.get("shot_note_bean") : "";
    float doseIn = params.has("shot_note_dose") ? params.get("shot_note_dose").toFloat() : 0.0f;
    String grind = params.has("shot_note_grind") ? params.get("shot_note_grind") : "";
    int rating = params.has("shot_note_rating") ? params.get("shot_note_rating").toInt() : 0;
    String notes = params.has("shot_note_text") ? params.get("shot_note_text") : "";
    shotLogUpdateNotes(idx, bean, doseIn, grind, rating, notes);
  }

  if (params.has("profile_apply")) {
    applyProfile(params.get("profile_apply").toInt());
  }
  if (params.has("profile_save")) {
    int idx = params.has("profile_index") ? params.get("profile_index").toInt() : -1;
    String name = params.has("profile_name") ? params.get("profile_name") : "Profile";
    double temp = params.has("profile_temp") ? params.get("profile_temp").toDouble() : BREW_SETPOINT_DEFAULT;
    unsigned long autoStop = params.has("profile_autostop")
        ? constrain((long)params.get("profile_autostop").toInt(), (long)SHOT_AUTO_STOP_SEC_MIN, (long)SHOT_AUTO_STOP_SEC_MAX)
        : SHOT_AUTO_STOP_SEC_DEFAULT;
    bool piEnabled = params.has("profile_pi_enabled") && params.get("profile_pi_enabled") == "1";
    int pulses = params.has("profile_pi_pulses")
        ? constrain(params.get("profile_pi_pulses").toInt(), 0, PREINFUSION_PULSES_MAX) : 0;
    int onMs = params.has("profile_pi_on_ms")
        ? constrain(params.get("profile_pi_on_ms").toInt(), PREINFUSION_PULSE_MS_MIN, PREINFUSION_PULSE_MS_MAX) : PREINFUSION_ON_MS_DEFAULT;
    int offMs = params.has("profile_pi_off_ms")
        ? constrain(params.get("profile_pi_off_ms").toInt(), PREINFUSION_PULSE_MS_MIN, PREINFUSION_PULSE_MS_MAX) : PREINFUSION_OFF_MS_DEFAULT;
    bool pressureEnabled = params.has("profile_press_enabled") && params.get("profile_press_enabled") == "1";
    double pressureRampBar = params.has("profile_press_ramp_bar")
        ? constrain(params.get("profile_press_ramp_bar").toDouble(), 0.0, PUMP_MAX_SAFETY_BAR - 1.0)
        : PRESSURE_RAMP_BAR_DEFAULT;
    unsigned long pressureRampMs = params.has("profile_press_ramp_ms")
        ? (unsigned long)constrain((long)params.get("profile_press_ramp_ms").toInt(), 0L, 120000L)
        : PRESSURE_RAMP_MS_DEFAULT;
    bool pressureDeclineEnabled = params.has("profile_press_decline_enabled") && params.get("profile_press_decline_enabled") == "1";
    double pressureDeclineBar = params.has("profile_press_decline_bar")
        ? constrain(params.get("profile_press_decline_bar").toDouble(), 0.0, PUMP_MAX_SAFETY_BAR - 1.0)
        : PRESSURE_DECLINE_BAR_DEFAULT;
    unsigned long pressureDeclineMs = params.has("profile_press_decline_ms")
        ? (unsigned long)constrain((long)params.get("profile_press_decline_ms").toInt(), 0L, 120000L)
        : PRESSURE_DECLINE_MS_DEFAULT;
    int saved = profileSave(idx, name, temp, autoStop, piEnabled, pulses, onMs, offMs,
                             pressureEnabled, pressureRampBar, pressureRampMs,
                             pressureDeclineEnabled, pressureDeclineBar, pressureDeclineMs);
    if (saved >= 0 && saved == activeProfileIndex) applyProfile(saved);
  }
  if (params.has("profile_delete")) {
    profileDelete(params.get("profile_delete").toInt());
  }

  for (int i = 0; i < SCHED_MAX_COUNT; i++) {
    String prefix = "sched" + String(i) + "_";
    String enArg = prefix + "en", timeArg = prefix + "time", steamArg = prefix + "steam";
    String enKey = "sched" + String(i) + "_en", hrKey = "sched" + String(i) + "_hr",
           mnKey = "sched" + String(i) + "_mn", stKey = "sched" + String(i) + "_st";
    if (params.has(enArg.c_str())) {
      schedEnabled[i] = params.get(enArg.c_str()) == "1";
      preferences.putBool(enKey.c_str(), schedEnabled[i]);
      resetSchedFired(i);
    }
    if (params.has(timeArg.c_str())) {
      String t = params.get(timeArg.c_str());
      int colon = t.indexOf(':');
      if (colon > 0) {
        schedHour[i] = constrain(t.substring(0, colon).toInt(), 0, 23);
        schedMin[i] = constrain(t.substring(colon + 1).toInt(), 0, 59);
        preferences.putInt(hrKey.c_str(), schedHour[i]);
        preferences.putInt(mnKey.c_str(), schedMin[i]);
        resetSchedFired(i);
      }
    }
    if (params.has(steamArg.c_str())) {
      schedModeSteam[i] = params.get(steamArg.c_str()) == "1";
      preferences.putBool(stKey.c_str(), schedModeSteam[i]);
    }
  }
  if (params.has("sched_tz_min")) {
    schedTzOffsetMin = params.get("sched_tz_min").toInt();
    preferences.putInt("sched_tz_min", schedTzOffsetMin);
  }

  if (params.has("mqtt_server")) {
    preferences.putString("mqtt_server", params.get("mqtt_server"));
  }
  if (params.has("mqtt_port")) {
    preferences.putInt("mqtt_port", params.get("mqtt_port").toInt());
  }
  if (params.has("mqtt_user")) {
    preferences.putString("mqtt_user", params.get("mqtt_user"));
  }
  if (params.has("mqtt_pass")) {
    preferences.putString("mqtt_pass", params.get("mqtt_pass"));
  }

  preferences.end();
  bool restartForMqtt = params.has("mqtt_server");

  unlockState();
  return restartForMqtt;
}

bool isKnownUpdateField(const String &name) {
  static const char *const kKnownFields[] = {
      "brew_target", "brew_kp", "brew_ki", "brew_kd",
      "brew_akp", "brew_aki", "brew_akd",
      "steam_target", "steam_kp", "steam_ki", "steam_kd",
      "press_kp", "press_ki", "press_kd",
      "steam_max_safety", "mode",
      "shot_auto_stop_sec", "eco_timeout_min", "steam_auto_off_min", "wake",
      "autotune", "shot",
      "mark_descaled", "descale_shot_threshold", "descale_day_threshold",
      "shot_note_index", "shot_note_bean", "shot_note_dose", "shot_note_grind",
      "shot_note_rating", "shot_note_text",
      "profile_apply", "profile_save", "profile_index", "profile_name",
      "profile_temp", "profile_autostop", "profile_pi_enabled",
      "profile_pi_pulses", "profile_pi_on_ms", "profile_pi_off_ms",
      "profile_press_enabled", "profile_press_ramp_bar", "profile_press_ramp_ms",
      "profile_press_decline_enabled", "profile_press_decline_bar",
      "profile_press_decline_ms", "profile_delete",
      "sched_tz_min", "mqtt_server", "mqtt_port", "mqtt_user", "mqtt_pass",
  };
  for (const char *f : kKnownFields) {
    if (name == f) return true;
  }
  // Per-slot schedule fields: sched0_en/sched0_time/sched0_steam, sched1_..., etc.
  for (int i = 0; i < SCHED_MAX_COUNT; i++) {
    String prefix = "sched" + String(i) + "_";
    if (name == prefix + "en" || name == prefix + "time" || name == prefix + "steam") {
      return true;
    }
  }
  return false;
}
```

(`buildStatusJson()` is added in Task 3 - this task only adds the two
`applyUpdateParams`/`isKnownUpdateField` functions declared above it in the
header.)

- [ ] **Step 3: Replace `handleUpdate()` in `web.cpp` with a thin wrapper**

In `src/web.cpp`, add `#include "command_dispatch.h"` alongside the other
includes near the top (`web.cpp:13-15`), then replace the entire
`handleUpdate()` function body (`web.cpp:1846-2150`, from `static void
handleUpdate(AsyncWebServerRequest *request) {` through its closing `}`)
with:

```cpp
class AsyncWebParamSource : public ParamSource {
 public:
  explicit AsyncWebParamSource(AsyncWebServerRequest *request) : request_(request) {}
  bool has(const char *name) const override { return request_->hasParam(name); }
  String get(const char *name) const override { return request_->getParam(name)->value(); }
 private:
  AsyncWebServerRequest *request_;
};

// Settings/action endpoint - every mode/tuning/shot/profile/schedule/MQTT
// change funnels through this one handler. The actual field-by-field
// validation/clamping/persistence lives in applyUpdateParams()
// (command_dispatch.cpp) so BLE/Serial commands go through exactly the same
// logic - see the connectivity protocol spec §7.
static void handleUpdate(AsyncWebServerRequest *request) {
  AsyncWebParamSource params(request);
  bool restartForMqtt = applyUpdateParams(params);

  request->redirect("/");

  if (restartForMqtt) {
    delay(500);
    ESP.restart();
  }
}
```

- [ ] **Step 4: Build**

Run: `& "C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\pio.exe" run -e esp32-s3-devkitc-1`
Expected: PASS.

- [ ] **Step 5: Bench regression check**

Flash (`-t upload`), then with the board on WiFi, confirm `/update` still
behaves identically to before the refactor - e.g. from a browser or
`curl "http://gaggia.local/update?mode=brew"` followed by `curl
http://gaggia.local/status`, confirm `opmode` reflects the change, then set
it back to `off`. This exercises the exact same code path BLE/Serial will
use later, so it's the one behavior-preserving check that matters most here.

- [ ] **Step 6: Commit**

```bash
git add include/command_dispatch.h src/command_dispatch.cpp src/web.cpp
git commit -m "Extract /update's field-dispatch logic into a transport-agnostic command_dispatch layer"
```

---

## Task 3: Shared status-building - `buildStatusJson()`

**Files:**
- Modify: `include/command_dispatch.h` (already declares `buildStatusJson` -
  no change needed, added in Task 2)
- Modify: `src/command_dispatch.cpp` (append `buildStatusJson()`)
- Modify: `src/web.cpp:1631-1841` (`handleStatus()` becomes a thin wrapper)

**Interfaces:**
- Consumes: same extern block as Task 2 (already present in
  `command_dispatch.cpp`), plus `dimmerGetPowerPercent()`/`dimmerGetZcCount()`
  from `dimmer.h` (already included in Task 2).
- Produces: `String buildStatusJson(bool includeHistory)`, used by
  `web.cpp`'s `/status` (Step 3 below, `includeHistory=true`) and later by
  the `{"cmd":"status"}` BLE/Serial command (Task 8, `includeHistory=false`).

- [ ] **Step 1: Append `buildStatusJson()` to `src/command_dispatch.cpp`**

Moves `handleStatus()`'s body from `web.cpp:1631-1841` essentially
unchanged - same snapshot-under-lock-then-build-JSON-unlocked pattern - with
the two history-array blocks gated on the new `includeHistory` parameter:

```cpp
String buildStatusJson(bool includeHistory) {
  lockState();
  float snapTemp = currentTemperature;
  double snapSetpoint = Setpoint, snapOutput = Output;
  OpMode snapMode = currentMode;
  double snapBrewTarget = brewSetpoint, snapBrewKp = brewKp, snapBrewKi = brewKi, snapBrewKd = brewKd;
  double snapBrewAkp = brewActiveKp, snapBrewAki = brewActiveKi, snapBrewAkd = brewActiveKd;
  double snapSteamTarget = steamSetpoint, snapSteamKp = steamKp, snapSteamKi = steamKi, snapSteamKd = steamKd;
  double snapSteamMaxSafety = steamMaxSafety;
  bool snapFault = sensorFault;
  unsigned long snapEcoTimeoutMin = ecoTimeoutMin;
  unsigned long snapSteamAutoOffMin = steamAutoOffMin;
  bool snapAutoSleeping = autoSleeping;
  OpMode snapModeBeforeSleep = modeBeforeSleep;
  unsigned long snapShotAutoStopSec = shotAutoStopSec;
  AutotuneState snapAutotuneState = autotuneState;
  String snapAutotuneMessage = autotuneMessage;
  bool snapShotInProgress = shotInProgress;
  unsigned long snapShotStartMillis = shotStartMillis;
  unsigned long snapShotCount = shotCount;
  time_t snapLastDescaleTime = lastDescaleTime;
  unsigned long snapDescaleShotThreshold = descaleShotThreshold;
  unsigned long snapDescaleDayThreshold = descaleDayThreshold;
  int snapActiveProfileIndex = activeProfileIndex;
  bool snapPiEnabled = activePreinfusionEnabled;
  int snapPiPulses = activePreinfusionPulses, snapPiOnMs = activePreinfusionOnMs, snapPiOffMs = activePreinfusionOffMs;
  ShotPhase snapShotPhase = currentShotPhase;
  bool snapSchedEnabled[SCHED_MAX_COUNT];
  int snapSchedHour[SCHED_MAX_COUNT], snapSchedMin[SCHED_MAX_COUNT];
  bool snapSchedModeSteam[SCHED_MAX_COUNT];
  for (int i = 0; i < SCHED_MAX_COUNT; i++) {
    snapSchedEnabled[i] = schedEnabled[i];
    snapSchedHour[i] = schedHour[i];
    snapSchedMin[i] = schedMin[i];
    snapSchedModeSteam[i] = schedModeSteam[i];
  }
  int snapHistoryCount = tempHistoryCount, snapHistoryHead = tempHistoryHead;
  float snapHistory[TEMP_HISTORY_LEN];
  for (int i = 0; i < snapHistoryCount; i++) {
    int idx = (snapHistoryHead - snapHistoryCount + i + TEMP_HISTORY_LEN * 2) % TEMP_HISTORY_LEN;
    snapHistory[i] = tempHistory[idx];
  }
  bool snapPressureFault = pressureFault;
  float snapPressure = currentPressure;
  bool snapPressEnabled = activePressureEnabled;
  double snapPressRampBar = activePressureRampBar;
  unsigned long snapPressRampMs = activePressureRampMs;
  bool snapPressDeclineEnabled = activePressureDeclineEnabled;
  double snapPressDeclineBar = activePressureDeclineBar;
  unsigned long snapPressDeclineMs = activePressureDeclineMs;
  double snapPressKp = pressureKp, snapPressKi = pressureKi, snapPressKd = pressureKd;
  float snapPumpPower = dimmerGetPowerPercent();
  uint32_t snapDimmerZcCount = dimmerGetZcCount();
  bool snapPressureCeilingTripped = (pressureFault || currentPressure > PUMP_MAX_SAFETY_BAR);
  int snapPressHistoryCount = pressureHistoryCount, snapPressHistoryHead = pressureHistoryHead;
  float snapPressHistory[TEMP_HISTORY_LEN];
  for (int i = 0; i < snapPressHistoryCount; i++) {
    int idx = (snapPressHistoryHead - snapPressHistoryCount + i + TEMP_HISTORY_LEN * 2) % TEMP_HISTORY_LEN;
    snapPressHistory[i] = pressureHistory[idx];
  }
  unlockState();

  String json = "{";
  json += "\"temp\":" + String(snapTemp);
  json += ",\"target\":" + String(snapSetpoint);
  json += ",\"output\":" + String(snapOutput);

  json += ",\"opmode\":\"";
  json += (snapMode == OpMode::BREW)    ? "brew"
           : (snapMode == OpMode::STEAM) ? "steam"
                                          : "off";
  json += "\"";

  json += ",\"brew_target\":" + String(snapBrewTarget);
  json += ",\"brew_kp\":" + String(snapBrewKp, 4);
  json += ",\"brew_ki\":" + String(snapBrewKi, 4);
  json += ",\"brew_kd\":" + String(snapBrewKd, 4);
  json += ",\"brew_akp\":" + String(snapBrewAkp, 4);
  json += ",\"brew_aki\":" + String(snapBrewAki, 4);
  json += ",\"brew_akd\":" + String(snapBrewAkd, 4);
  json += ",\"steam_target\":" + String(snapSteamTarget);
  json += ",\"steam_kp\":" + String(snapSteamKp, 4);
  json += ",\"steam_ki\":" + String(snapSteamKi, 4);
  json += ",\"steam_kd\":" + String(snapSteamKd, 4);
  json += ",\"steam_max_safety\":" + String(snapSteamMaxSafety);

  Preferences preferences;
  preferences.begin("gaggia", true);
  json +=
      ",\"mqtt_server\":\"" + preferences.getString("mqtt_server", "") + "\"";
  json += ",\"mqtt_port\":" + String(preferences.getInt("mqtt_port", 1883));
  json += ",\"mqtt_user\":\"" + preferences.getString("mqtt_user", "") + "\"";
  json += ",\"mqtt_pass\":\"" + preferences.getString("mqtt_pass", "") + "\"";
  preferences.end();

  json += ",\"fw_build\":\"" + String(FIRMWARE_BUILD_TIMESTAMP) + "\"";

  json += ",\"fault\":" + String(snapFault ? "true" : "false");

  time_t nowEpoch = time(nullptr);
  json += ",\"ntp_synced\":" + String(nowEpoch > 1600000000L ? "true" : "false");
  json += ",\"server_time\":" + String((long long)nowEpoch);

  json += ",\"eco_timeout_min\":" + String(snapEcoTimeoutMin);
  json += ",\"steam_auto_off_min\":" + String(snapSteamAutoOffMin);
  json += ",\"auto_sleeping\":" + String(snapAutoSleeping ? "true" : "false");
  json += ",\"asleep_from\":\"";
  json += (snapModeBeforeSleep == OpMode::STEAM) ? "steam" : "brew";
  json += "\"";
  json += ",\"shot_auto_stop_sec\":" + String(snapShotAutoStopSec);

  json += ",\"autotune_state\":\"";
  switch (snapAutotuneState) {
    case AutotuneState::RUNNING: json += "running"; break;
    case AutotuneState::DONE_OK: json += "done_ok"; break;
    case AutotuneState::DONE_FAIL: json += "done_fail"; break;
    default: json += "idle"; break;
  }
  json += "\"";
  json += ",\"autotune_message\":\"" + snapAutotuneMessage + "\"";

  json += ",\"shot_in_progress\":" + String(snapShotInProgress ? "true" : "false");
  json += ",\"shot_elapsed_ms\":" +
          String(snapShotInProgress ? (millis() - snapShotStartMillis) : 0);

  json += ",\"shot_count\":" + String(snapShotCount);
  json += ",\"descale_shot_threshold\":" + String(snapDescaleShotThreshold);
  json += ",\"descale_day_threshold\":" + String(snapDescaleDayThreshold);
  long daysSinceDescale =
      (snapLastDescaleTime > 0) ? (long)((time(nullptr) - snapLastDescaleTime) / 86400L) : -1;
  json += ",\"days_since_descale\":" + String(daysSinceDescale);
  bool descaleDue =
      (snapShotCount >= snapDescaleShotThreshold) ||
      (daysSinceDescale >= 0 && (unsigned long)daysSinceDescale >= snapDescaleDayThreshold);
  json += ",\"descale_due\":" + String(descaleDue ? "true" : "false");

  json += ",\"active_profile\":" + String(snapActiveProfileIndex);
  json += ",\"pi_enabled\":" + String(snapPiEnabled ? "true" : "false");
  json += ",\"pi_pulses\":" + String(snapPiPulses);
  json += ",\"pi_on_ms\":" + String(snapPiOnMs);
  json += ",\"pi_off_ms\":" + String(snapPiOffMs);
  json += ",\"pressure\":" + String(snapPressure, 2);
  json += ",\"pressure_fault\":" + String(snapPressureFault ? "true" : "false");
  json += ",\"pump_power\":" + String(snapPumpPower, 1);
  json += ",\"dimmer_zc_count\":" + String(snapDimmerZcCount);
  json += ",\"pressure_ceiling_tripped\":" + String(snapPressureCeilingTripped ? "true" : "false");
  json += ",\"press_enabled\":" + String(snapPressEnabled ? "true" : "false");
  json += ",\"press_ramp_bar\":" + String(snapPressRampBar);
  json += ",\"press_ramp_ms\":" + String(snapPressRampMs);
  json += ",\"press_decline_enabled\":" + String(snapPressDeclineEnabled ? "true" : "false");
  json += ",\"press_decline_bar\":" + String(snapPressDeclineBar);
  json += ",\"press_decline_ms\":" + String(snapPressDeclineMs);
  json += ",\"press_kp\":" + String(snapPressKp, 4);
  json += ",\"press_ki\":" + String(snapPressKi, 4);
  json += ",\"press_kd\":" + String(snapPressKd, 4);
  json += ",\"shot_phase\":\"";
  switch (snapShotPhase) {
    case ShotPhase::PREINFUSION_ON:
    case ShotPhase::PREINFUSION_OFF: json += "preinfusion"; break;
    case ShotPhase::PRESSURE: json += "pressure"; break;
    case ShotPhase::EXTRACTION: json += "extraction"; break;
    default: json += "none"; break;
  }
  json += "\"";

  json += ",\"sched\":[";
  for (int i = 0; i < SCHED_MAX_COUNT; i++) {
    if (i > 0) json += ",";
    json += "{\"en\":" + String(snapSchedEnabled[i] ? "true" : "false") +
            ",\"hr\":" + String(snapSchedHour[i]) + ",\"mn\":" + String(snapSchedMin[i]) +
            ",\"st\":" + String(snapSchedModeSteam[i] ? "true" : "false") + "}";
  }
  json += "]";
  json += ",\"sched_tz_min\":" + String(schedTzOffsetMin);

  if (includeHistory) {
    json += ",\"history\":[";
    for (int i = 0; i < snapHistoryCount; i++) {
      if (i > 0) json += ",";
      json += String(snapHistory[i], 1);
    }
    json += "]";
    json += ",\"pressure_history\":[";
    for (int i = 0; i < snapPressHistoryCount; i++) {
      if (i > 0) json += ",";
      json += String(snapPressHistory[i], 2);
    }
    json += "]";
  }

  json += "}";
  return json;
}
```

- [ ] **Step 2: Replace `handleStatus()` in `web.cpp` with a thin wrapper**

Replace `src/web.cpp:1631-1841` (the entire `handleStatus()` function) with:

```cpp
static void handleStatus(AsyncWebServerRequest *request) {
  request->send(200, "application/json", buildStatusJson(true));
}
```

- [ ] **Step 3: Build**

Run: `& "C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\pio.exe" run -e esp32-s3-devkitc-1`
Expected: PASS.

- [ ] **Step 4: Bench regression check**

Flash, then `curl http://gaggia.local/status` and diff the field set/shape
against what the Web UI showed before this change (same fields, same
values) - confirms the refactor didn't drop or rename anything the frontend
depends on.

- [ ] **Step 5: Commit**

```bash
git add src/command_dispatch.cpp src/web.cpp
git commit -m "Extract /status's JSON-building into a shared buildStatusJson()"
```

---

## Task 4: Binary telemetry frame encoder

**Files:**
- Create: `include/telemetry.h`
- Create: `src/telemetry.cpp`

**Interfaces:**
- Produces: `void encodeTelemetryFrame(uint8_t out[TELEMETRY_FRAME_LEN])`,
  `uint16_t crc16Ccitt(const uint8_t *data, size_t len)`.
- Consumes: `currentTemperature`, `currentPressure`, `Output`, `WindowSize`,
  `currentMode`, `currentShotPhase`, `sensorFault`, `pressureFault`,
  `shotInProgress`, `autoSleeping`, `shotStartMillis`, `shotCount`,
  `descaleShotThreshold`, `descaleDayThreshold`, `lastDescaleTime` (all
  `extern` from `main.cpp`), `dimmerGetPowerPercent()` (`dimmer.h`),
  `lockState()`/`unlockState()`.

- [ ] **Step 1: Create `include/telemetry.h`**

```cpp
#pragma once

#include <stdint.h>
#include <stddef.h>

// CRC16-CCITT (poly 0x1021, init 0xFFFF, no reflect) - the same well-known
// variant BLE's own link layer uses elsewhere, kept here as a plain
// function since Serial (unlike BLE) has no link-layer CRC of its own
// (spec §4a).
uint16_t crc16Ccitt(const uint8_t *data, size_t len);

// Fills `out` (must be TELEMETRY_FRAME_LEN bytes) with the current live
// telemetry frame - see the connectivity protocol spec §4a for the exact
// byte layout. Safe to call from any task; snapshots every field under
// main.cpp's stateMutex the same way buildStatusJson() does.
void encodeTelemetryFrame(uint8_t *out);
```

- [ ] **Step 2: Create `src/telemetry.cpp`**

```cpp
#include "telemetry.h"

#include <Arduino.h>
#include <string.h>

#include "config.h"
#include "dimmer.h"

extern float currentTemperature;
extern float currentPressure;
extern double Output;
extern int WindowSize;
extern OpMode currentMode;
extern ShotPhase currentShotPhase;
extern bool sensorFault;
extern bool pressureFault;
extern bool shotInProgress;
extern bool autoSleeping;
extern unsigned long shotStartMillis;
extern unsigned long shotCount;
extern unsigned long descaleShotThreshold;
extern unsigned long descaleDayThreshold;
extern time_t lastDescaleTime;

extern void lockState();
extern void unlockState();

uint16_t crc16Ccitt(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

static void writeFloatLE(uint8_t *out, float v) {
  uint32_t bits;
  memcpy(&bits, &v, sizeof(bits));
  out[0] = (uint8_t)(bits & 0xFF);
  out[1] = (uint8_t)((bits >> 8) & 0xFF);
  out[2] = (uint8_t)((bits >> 16) & 0xFF);
  out[3] = (uint8_t)((bits >> 24) & 0xFF);
}

static void writeU32LE(uint8_t *out, uint32_t v) {
  out[0] = (uint8_t)(v & 0xFF);
  out[1] = (uint8_t)((v >> 8) & 0xFF);
  out[2] = (uint8_t)((v >> 16) & 0xFF);
  out[3] = (uint8_t)((v >> 24) & 0xFF);
}

void encodeTelemetryFrame(uint8_t *out) {
  lockState();
  float snapTemp = currentTemperature;
  float snapPressure = currentPressure;
  double snapOutput = Output;
  int snapWindowSize = WindowSize;
  float snapPumpPower = dimmerGetPowerPercent();
  OpMode snapMode = currentMode;
  ShotPhase snapPhase = currentShotPhase;
  bool snapSensorFault = sensorFault;
  bool snapPressureFault = pressureFault;
  bool snapShotInProgress = shotInProgress;
  bool snapAutoSleeping = autoSleeping;
  unsigned long snapShotStartMillis = shotStartMillis;
  unsigned long snapShotCount = shotCount;
  unsigned long snapDescaleShotThreshold = descaleShotThreshold;
  unsigned long snapDescaleDayThreshold = descaleDayThreshold;
  time_t snapLastDescaleTime = lastDescaleTime;
  unlockState();

  bool pressureCeilingTripped = snapPressureFault || snapPressure > PUMP_MAX_SAFETY_BAR;
  long daysSinceDescale =
      (snapLastDescaleTime > 0) ? (long)((time(nullptr) - snapLastDescaleTime) / 86400L) : -1;
  bool descaleDue =
      (snapShotCount >= snapDescaleShotThreshold) ||
      (daysSinceDescale >= 0 && (unsigned long)daysSinceDescale >= snapDescaleDayThreshold);

  // Byte 0 doubles as the protocol version (no separate version byte) -
  // keeps the frame at exactly 20 bytes, BLE's guaranteed-available
  // payload at the default, un-negotiated 23-byte ATT MTU (MTU - 3 bytes
  // of ATT overhead) - see config.h's TELEMETRY_FRAME_LEN comment for why
  // this matters (BluetoothGatt.requestMtu() doesn't exist before Android
  // API 21, so a KitKat client can never negotiate more than this).
  out[0] = TELEMETRY_SYNC_MARKER;
  writeFloatLE(out + 1, snapTemp);
  writeFloatLE(out + 5, snapPressure);
  uint8_t outputPercent = (uint8_t)constrain((int)(snapOutput * 100.0 / snapWindowSize), 0, 100);
  out[9] = outputPercent;
  out[10] = (uint8_t)constrain((int)(snapPumpPower + 0.5f), 0, 100);
  out[11] = (uint8_t)snapMode;
  out[12] = (uint8_t)snapPhase;

  uint8_t flags = 0;
  if (snapSensorFault) flags |= (1 << 0);
  if (snapPressureFault) flags |= (1 << 1);
  if (snapShotInProgress) flags |= (1 << 2);
  if (snapAutoSleeping) flags |= (1 << 3);
  if (pressureCeilingTripped) flags |= (1 << 4);
  if (descaleDue) flags |= (1 << 5);
  out[13] = flags;

  uint32_t shotElapsedMs = snapShotInProgress ? (uint32_t)(millis() - snapShotStartMillis) : 0;
  writeU32LE(out + 14, shotElapsedMs);

  uint16_t crc = crc16Ccitt(out, 18);
  out[18] = (uint8_t)(crc & 0xFF);
  out[19] = (uint8_t)((crc >> 8) & 0xFF);
}
```

- [ ] **Step 3: Build**

Run: `& "C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\pio.exe" run -e esp32-s3-devkitc-1`
Expected: PASS. Nothing calls `encodeTelemetryFrame()` yet (wired in Task
8) - this task only needs to compile and link cleanly.

- [ ] **Step 4: Commit**

```bash
git add include/telemetry.h src/telemetry.cpp
git commit -m "Add the 20-byte binary telemetry frame encoder and its CRC16"
```

---

## Task 5: Serial transport (line-buffered JSON + raw binary writes)

**Files:**
- Create: `include/serial_transport.h`
- Create: `src/serial_transport.cpp`

**Interfaces:**
- Produces: `bool serialReadJsonLine(String &out)`, `void
  serialWriteJsonLine(const String &json)`, `void
  serialWriteTelemetryFrame(const uint8_t *frame, size_t len)`.

**Design note:** the same USB-CDC Serial link already carries plenty of
plain-text `Serial.println(...)` debug output from `controlLoopTask` (shot
auto-stop, autotune progress, sensor fault messages, etc.) - this plan
doesn't touch any of that logging. A real client reading this link must
already treat it as "JSON lines and 0xA5-prefixed binary frames, ignore
anything else" rather than assuming every line is protocol traffic; Task 9's
smoke-test script does exactly that, and is the reference for how a client
should behave.

- [ ] **Step 1: Create `include/serial_transport.h`**

```cpp
#pragma once

#include <Arduino.h>

// Line-buffered Serial JSON command reader, following the same
// "line-buffered, only process on \n" lesson temp_sensor.cpp's protocol
// reverse-engineering already documented for this project's UART sensor
// module. Non-blocking - call every connectivity-task tick; returns true
// (with `out` set) at most once per complete, non-empty, '{'-prefixed line
// currently buffered. Any other line (blank, or not starting with '{' - see
// the design note above) is silently discarded.
bool serialReadJsonLine(String &out);

// Writes one JSON response/command as its own line (newline-terminated).
void serialWriteJsonLine(const String &json);

// Writes a raw binary telemetry frame (not newline-terminated - the fixed
// TELEMETRY_FRAME_LEN length is the frame boundary, distinguished from a
// JSON line by its leading TELEMETRY_SYNC_MARKER byte, spec §4a).
void serialWriteTelemetryFrame(const uint8_t *frame, size_t len);
```

- [ ] **Step 2: Create `src/serial_transport.cpp`**

```cpp
#include "serial_transport.h"

#include "config.h"

static String serialLineBuffer;

bool serialReadJsonLine(String &out) {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n') {
      String line = serialLineBuffer;
      line.trim(); // drops a trailing \r plus any stray whitespace
      serialLineBuffer = "";
      if (line.length() > 0 && line[0] == '{') {
        out = line;
        return true;
      }
      continue; // not a JSON command line - discard, keep reading
    }
    serialLineBuffer += c;
    if (serialLineBuffer.length() > SERIAL_MAX_LINE_LEN) {
      // Runaway line (no '\n' ever arrived) - drop it so a stray byte
      // stream can't grow this buffer unbounded.
      serialLineBuffer = "";
    }
  }
  return false;
}

void serialWriteJsonLine(const String &json) {
  Serial.println(json);
}

void serialWriteTelemetryFrame(const uint8_t *frame, size_t len) {
  Serial.write(frame, len);
}
```

- [ ] **Step 3: Build**

Run: `& "C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\pio.exe" run -e esp32-s3-devkitc-1`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add include/serial_transport.h src/serial_transport.cpp
git commit -m "Add the line-buffered Serial JSON/binary telemetry transport"
```

---

## Task 6: BLE transport (NimBLE GATT service)

**Files:**
- Create: `include/ble_transport.h`
- Create: `src/ble_transport.cpp`

**Interfaces:**
- Produces: `void bleTransportInit()`, `bool bleReadCommand(String &out)`,
  `void bleSendResponse(const String &json)`, `void bleSendTelemetry(const
  uint8_t *frame, size_t len)`.
- Consumes: `BLE_DEVICE_NAME`, `BLE_SERVICE_UUID`,
  `BLE_CHAR_TELEMETRY_UUID`, `BLE_CHAR_COMMAND_UUID`,
  `BLE_CHAR_RESPONSE_UUID`, `BLE_MAX_COMMAND_LEN` (Task 1's `config.h`
  additions).

- [ ] **Step 1: Create `include/ble_transport.h`**

```cpp
#pragma once

#include <Arduino.h>

// NimBLE GATT transport - one custom primary service with three
// characteristics (Telemetry/Notify, Command/Write, Response/Notify), see
// the connectivity protocol spec §5. NimBLE-Arduino was chosen over the
// built-in Bluedroid stack specifically for WiFi coexistence (spec §3) -
// this runs alongside the existing WiFi Web UI, not instead of it.

// Starts the BLE stack, creates the service/characteristics, and starts
// advertising. Call once from setup() (via connectivityInit()).
void bleTransportInit();

// Pops the oldest queued command write, if any (non-blocking). Returns
// false if no command is queued.
bool bleReadCommand(String &out);

// Sets the Response characteristic's value and notifies any subscribed
// central.
void bleSendResponse(const String &json);

// Sets the Telemetry characteristic's value and notifies any subscribed
// central.
void bleSendTelemetry(const uint8_t *frame, size_t len);
```

- [ ] **Step 2: Create `src/ble_transport.cpp`**

```cpp
#include "ble_transport.h"

#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>

#include "config.h"

struct BleCommandMsg {
  uint16_t len;
  char data[BLE_MAX_COMMAND_LEN];
};

static QueueHandle_t bleCommandQueue = nullptr;
static NimBLECharacteristic *telemetryChar = nullptr;
static NimBLECharacteristic *commandChar = nullptr;
static NimBLECharacteristic *responseChar = nullptr;

class CommandWriteCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *chr) override {
    std::string v = chr->getValue();
    // Drop empty/oversized writes rather than silently truncate a JSON
    // command - the client will see no response and can retry/inspect.
    if (v.empty() || v.size() >= BLE_MAX_COMMAND_LEN) return;
    BleCommandMsg msg;
    msg.len = (uint16_t)v.size();
    memcpy(msg.data, v.data(), v.size());
    msg.data[v.size()] = '\0';
    // Never block the NimBLE host task - drop on a full queue (a client
    // sending commands faster than the connectivity task drains them is
    // already misbehaving; dropping is safer than stalling BLE's own task).
    xQueueSend(bleCommandQueue, &msg, 0);
  }
};

void bleTransportInit() {
  bleCommandQueue = xQueueCreate(8, sizeof(BleCommandMsg));

  NimBLEDevice::init(BLE_DEVICE_NAME);
  NimBLEServer *server = NimBLEDevice::createServer();

  NimBLEService *service = server->createService(BLE_SERVICE_UUID);
  telemetryChar = service->createCharacteristic(BLE_CHAR_TELEMETRY_UUID, NIMBLE_PROPERTY::NOTIFY);
  commandChar = service->createCharacteristic(BLE_CHAR_COMMAND_UUID, NIMBLE_PROPERTY::WRITE);
  responseChar = service->createCharacteristic(BLE_CHAR_RESPONSE_UUID, NIMBLE_PROPERTY::NOTIFY);
  commandChar->setCallbacks(new CommandWriteCallbacks());

  service->start();

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->start();
}

bool bleReadCommand(String &out) {
  BleCommandMsg msg;
  if (xQueueReceive(bleCommandQueue, &msg, 0) != pdTRUE) return false;
  out = String(msg.data);
  return true;
}

void bleSendResponse(const String &json) {
  if (!responseChar) return;
  responseChar->setValue((const uint8_t *)json.c_str(), json.length());
  responseChar->notify();
}

void bleSendTelemetry(const uint8_t *frame, size_t len) {
  if (!telemetryChar) return;
  telemetryChar->setValue(frame, len);
  telemetryChar->notify();
}
```

- [ ] **Step 3: Build**

Run: `& "C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\pio.exe" run -e esp32-s3-devkitc-1`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add include/ble_transport.h src/ble_transport.cpp
git commit -m "Add the NimBLE GATT transport (telemetry/command/response characteristics)"
```

---

## Task 7: Connectivity task - JSON command dispatch + task wiring

**Files:**
- Create: `include/connectivity.h`
- Create: `src/connectivity.cpp`

**Interfaces:**
- Produces: `void connectivityInit()`.
- Consumes: `applyUpdateParams()`/`isKnownUpdateField()`/`buildStatusJson()`
  (Task 2/3), `encodeTelemetryFrame()` (Task 4), `serialReadJsonLine()`/
  `serialWriteJsonLine()`/`serialWriteTelemetryFrame()` (Task 5),
  `bleTransportInit()`/`bleReadCommand()`/`bleSendResponse()`/
  `bleSendTelemetry()` (Task 6), `profilesReadJson()` (`profiles.h`),
  `shotLogReadJson()` (`shot_log.h`).

- [ ] **Step 1: Create `include/connectivity.h`**

```cpp
#pragma once

// Starts the BLE transport and the dedicated connectivity FreeRTOS task
// (pinned to core 0, see config.h). Call once from setup(), after the
// control loop task has been created.
void connectivityInit();
```

- [ ] **Step 2: Create `src/connectivity.cpp`**

```cpp
#include "connectivity.h"

#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "ble_transport.h"
#include "command_dispatch.h"
#include "config.h"
#include "profiles.h"
#include "serial_transport.h"
#include "shot_log.h"
#include "telemetry.h"

// Adapts a parsed JSON "params" object to the same ParamSource interface
// HTTP's AsyncWebParamSource (web.cpp) implements - see command_dispatch.h.
// A JSON boolean is mapped to "1"/"0" (not ArduinoJson's default "true"/
// "false" stringification) so the moved /update logic's existing `== "1"`
// checks (wake, mark_descaled, profile_pi_enabled, etc.) keep working
// whether a client sends `"wake":"1"` (HTTP-form style) or `"wake":true`
// (native JSON boolean).
class JsonParamSource : public ParamSource {
 public:
  explicit JsonParamSource(JsonObjectConst params) : params_(params) {}
  bool has(const char *name) const override { return !params_[name].isNull(); }
  String get(const char *name) const override {
    JsonVariantConst v = params_[name];
    if (v.is<bool>()) return v.as<bool>() ? "1" : "0";
    return v.as<String>();
  }
 private:
  JsonObjectConst params_;
};

static String errorResponse(long id, bool hasId, const String &message) {
  JsonDocument doc;
  if (hasId) doc["id"] = id;
  doc["ok"] = false;
  doc["error"] = message;
  String out;
  serializeJson(doc, out);
  return out;
}

// Parses one JSON command line/write and returns its JSON response. Sets
// `restartRequired` (default false) if the caller should restart the board
// after sending that response - mirrors handleUpdate()'s existing
// "mqtt_server changed -> reboot" behavior (command_dispatch.h).
static String dispatchJsonCommand(const String &line, bool &restartRequired) {
  restartRequired = false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) return errorResponse(0, false, "malformed JSON");

  bool hasId = !doc["id"].isNull();
  long id = hasId ? doc["id"].as<long>() : 0;
  String cmd = doc["cmd"] | "";

  if (cmd == "status") {
    return buildStatusJson(false);
  }
  if (cmd == "profiles") {
    return profilesReadJson();
  }
  if (cmd == "history") {
    return shotLogReadJson();
  }
  if (cmd == "update") {
    JsonObjectConst params = doc["params"].as<JsonObjectConst>();
    for (JsonPairConst kv : params) {
      String key = kv.key().c_str();
      if (!isKnownUpdateField(key)) {
        return errorResponse(id, hasId, "unknown field: " + key);
      }
    }
    JsonParamSource source(params);
    restartRequired = applyUpdateParams(source);
    JsonDocument resp;
    if (hasId) resp["id"] = id;
    resp["ok"] = true;
    String out;
    serializeJson(resp, out);
    return out;
  }

  return errorResponse(id, hasId, "unknown cmd");
}

static void connectivityTaskEntry(void *pv) {
  TickType_t lastTelemetry = xTaskGetTickCount();
  for (;;) {
    String serialLine;
    while (serialReadJsonLine(serialLine)) {
      bool restart = false;
      String resp = dispatchJsonCommand(serialLine, restart);
      serialWriteJsonLine(resp);
      if (restart) {
        delay(500);
        ESP.restart();
      }
    }

    String bleCmd;
    while (bleReadCommand(bleCmd)) {
      bool restart = false;
      String resp = dispatchJsonCommand(bleCmd, restart);
      bleSendResponse(resp);
      if (restart) {
        delay(500);
        ESP.restart();
      }
    }

    if (xTaskGetTickCount() - lastTelemetry >= pdMS_TO_TICKS(CONNECTIVITY_TELEMETRY_INTERVAL_MS)) {
      lastTelemetry = xTaskGetTickCount();
      uint8_t frame[TELEMETRY_FRAME_LEN];
      encodeTelemetryFrame(frame);
      serialWriteTelemetryFrame(frame, TELEMETRY_FRAME_LEN);
      bleSendTelemetry(frame, TELEMETRY_FRAME_LEN);
    }

    vTaskDelay(pdMS_TO_TICKS(CONNECTIVITY_TASK_POLL_MS));
  }
}

void connectivityInit() {
  bleTransportInit();
  xTaskCreatePinnedToCore(connectivityTaskEntry, "connectivity",
                           CONNECTIVITY_TASK_STACK_SIZE, nullptr,
                           CONNECTIVITY_TASK_PRIORITY, nullptr, 0);
}
```

- [ ] **Step 3: Build**

Run: `& "C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\pio.exe" run -e esp32-s3-devkitc-1`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add include/connectivity.h src/connectivity.cpp
git commit -m "Add the connectivity task: JSON command dispatch over Serial and BLE"
```

---

## Task 8: Wire the connectivity task into `setup()`

**Files:**
- Modify: `src/main.cpp:1-16` (includes)
- Modify: `src/main.cpp:920-924` (end of `setup()`)

**Interfaces:**
- Consumes: `connectivityInit()` (Task 7).

- [ ] **Step 1: Include and call `connectivityInit()`**

In `src/main.cpp`, add `#include "connectivity.h"` to the include block at
the top (`main.cpp:1-8`, alongside the other project headers).

Then, at the end of `setup()` (`main.cpp:920-924`), right after the existing
`xTaskCreatePinnedToCore(controlLoopTask, ...)` call:

```cpp
  // Pinned to core 1, away from the WiFi/AsyncTCP stack's usual core 0 work,
  // same core-separation principle GaggiMate uses for its brew-logic task.
  xTaskCreatePinnedToCore(controlLoopTask, "control", CONTROL_TASK_STACK_SIZE,
                           nullptr, configMAX_PRIORITIES - 2, nullptr, 1);

  // BLE + Serial connectivity protocol (spec:
  // docs/superpowers/specs/2026-09-04-connectivity-protocol-design.md) -
  // its own task, pinned to core 0, priority between the Arduino loop
  // task's and controlLoopTask's (config.h). Runs alongside WiFi/the
  // existing Web UI, not instead of it.
  connectivityInit();
}
```

- [ ] **Step 2: Build**

Run: `& "C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\pio.exe" run -e esp32-s3-devkitc-1`
Expected: PASS.

- [ ] **Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "Start the BLE/Serial connectivity task from setup()"
```

---

## Task 9: Bench Milestone A - Serial protocol verification

**Files:**
- Create: `tools/connectivity_smoke_test.py`

**Interfaces:**
- Consumes: nothing (standalone bench tool, same pattern as
  `tools/serial_capture.py`).

- [ ] **Step 1: Create `tools/connectivity_smoke_test.py`**

Follows this project's existing `tools/serial_capture.py` pattern
(self-closing pyserial script, doesn't hold the port open). Sends a
`{"cmd":"status"}` command and an `{"cmd":"update",...}` command, and
separately validates one binary telemetry frame's CRC - exercising exactly
the framing (`{` vs. `0xA5`) a real client must implement (see Task 5's
design note: skip any line that's neither).

```python
"""Connectivity protocol smoke test (Serial transport).

Opens a COM port, sends a couple of JSON commands, prints the responses,
and validates one binary telemetry frame's CRC16 - a minimal bench check
that the Serial half of the BLE/Serial connectivity protocol
(docs/superpowers/specs/2026-09-04-connectivity-protocol-design.md) is
actually working end to end. Self-closing, like tools/serial_capture.py -
doesn't hold the port open.

Usage:  python connectivity_smoke_test.py [PORT] [BAUD]
Default: COM6 115200
"""
import json
import sys
import time

try:
    import serial  # pyserial (ships with PlatformIO's penv)
except ImportError:
    print("pyserial not available in this interpreter", flush=True)
    sys.exit(2)

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

SYNC_MARKER = 0xA5
FRAME_LEN = 20


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


ser = serial.Serial()
ser.port = port
ser.baudrate = baud
ser.timeout = 0.5
try:
    ser.dtr = False
    ser.rts = False
except Exception:
    pass
ser.open()
try:
    ser.dtr = False
    ser.rts = False
except Exception:
    pass

print(f"[smoke test] {port} @ {baud}", flush=True)


def read_response(deadline_s=3.0):
    """Reads bytes until a JSON line OR one full binary frame is found.
    Discards anything else (plain-text debug logs sharing the same port -
    see Task 5's design note), exactly like a real client should."""
    end = time.time() + deadline_s
    buf = bytearray()
    while time.time() < end:
        chunk = ser.read(256)
        if not chunk:
            continue
        buf.extend(chunk)
        while buf:
            if buf[0] == SYNC_MARKER:
                if len(buf) < FRAME_LEN:
                    break
                frame, buf = bytes(buf[:FRAME_LEN]), buf[FRAME_LEN:]
                return ("frame", frame)
            nl = buf.find(b"\n")
            if nl == -1:
                if buf[0:1] != b"{" and len(buf) > 0 and buf[0:1] not in (b"\r",):
                    # Not JSON and not a frame start - a plain-text debug
                    # line; drop everything up to (and excluding) the next
                    # '\n' or sync marker.
                    pass
                break
            line, buf = bytes(buf[:nl]), buf[nl + 1:]
            line = line.strip()
            if line.startswith(b"{"):
                return ("json", line)
            # else: discard (plain-text debug log line) and keep scanning
    return (None, None)


try:
    ser.write(b'{"id":1,"cmd":"status"}\n')
    kind, payload = read_response()
    assert kind == "json", f"expected a JSON response to status, got {kind!r}"
    status = json.loads(payload)
    assert "history" not in status, "cmd:status must omit the history arrays"
    print("[smoke test] status OK:", {k: status[k] for k in ("temp", "opmode", "fw_build")})

    ser.write(b'{"id":2,"cmd":"update","params":{"eco_timeout_min":31}}\n')
    kind, payload = read_response()
    assert kind == "json", f"expected a JSON response to update, got {kind!r}"
    resp = json.loads(payload)
    assert resp.get("ok") is True and resp.get("id") == 2, f"unexpected update response: {resp!r}"
    print("[smoke test] update OK:", resp)

    ser.write(b'{"id":3,"cmd":"update","params":{"not_a_real_field":1}}\n')
    kind, payload = read_response()
    assert kind == "json", f"expected a JSON response to the bad update, got {kind!r}"
    resp = json.loads(payload)
    assert resp.get("ok") is False, f"expected an error for an unknown field, got {resp!r}"
    print("[smoke test] unknown-field rejection OK:", resp)

    kind, payload = read_response(deadline_s=3.0)
    assert kind == "frame", f"expected a telemetry frame, got {kind!r}"
    crc_in_frame = payload[18] | (payload[19] << 8)
    assert crc16_ccitt(payload[:18]) == crc_in_frame, "telemetry frame CRC mismatch"
    print(f"[smoke test] telemetry frame OK: {len(payload)} bytes, CRC valid")

    print("[smoke test] ALL CHECKS PASSED")
finally:
    ser.close()
```

- [ ] **Step 2: Run the bench check**

Build + flash the board (`-t upload`), let it settle on WiFi for a few
seconds, then run:

```powershell
& "C:\Users\benny.koren\Desktop\esp32esso\.venv\Scripts\python.exe" tools\connectivity_smoke_test.py COM5 115200
```

(substitute the actual COM port - see `AGENTS.md` §5, "last seen on COM5").
Expected: `ALL CHECKS PASSED`, confirming: a `{"cmd":"status"}` request
returns status JSON without the history arrays, an `{"cmd":"update",...}`
request is applied and acknowledged, an unrecognized field is rejected with
`"ok":false`, and a telemetry frame with a valid CRC16 arrives within the
configured interval.

- [ ] **Step 3: Commit**

```bash
git add tools/connectivity_smoke_test.py
git commit -m "Add a Serial-transport smoke test for the connectivity protocol"
```

---

## Task 10: Bench Milestone B - BLE verification + WiFi/BLE coexistence

**Files:** none (manual bench verification only, matching the prior
pump-pressure plan's bring-up milestone tasks - no code change).

- [ ] **Step 1: Discover the service**

With the board flashed (Task 8/9 already done) and powered, open a BLE
scanner app (e.g. nRF Connect for Android/iOS) and confirm a device named
`GaggiaBrewMaster` is advertising, with one custom service
(`7a2f9c00-6b3e-4f1a-9d3b-2a0e4f6b8c10`) containing three characteristics
matching `BLE_CHAR_TELEMETRY_UUID`/`BLE_CHAR_COMMAND_UUID`/
`BLE_CHAR_RESPONSE_UUID` from `config.h`, with Notify/Write/Notify
properties respectively.

- [ ] **Step 2: Verify telemetry**

Subscribe (enable notifications) on the Telemetry characteristic. Confirm a
20-byte value arrives roughly every 2 seconds (`CONNECTIVITY_TELEMETRY_INTERVAL_MS`)
**without ever needing an MTU request** - the scanner app should show the
full 20 bytes at the connection's default MTU, confirming the frame fits
without negotiation (the whole point of the 2026-09-15 fix - see spec
§4a). Spot-check a couple of fields by hand against what `/status` (Web UI
or `curl`) shows at the same moment: byte 0 = `0xA5`, bytes 1-4 as a
little-endian float32 should match the current temperature, byte 11 should
be `0`/`1`/`2` for off/brew/steam matching `opmode`.

- [ ] **Step 3: Verify commands**

Subscribe to the Response characteristic, then write
`{"id":9,"cmd":"update","params":{"eco_timeout_min":32}}` (as UTF-8 bytes,
no trailing newline needed for BLE) to the Command characteristic. Confirm
a Response notification arrives with `{"id":9,"ok":true}`, and that
`/status` (over WiFi, in parallel) now shows `eco_timeout_min: 32`.

- [ ] **Step 4: Confirm WiFi + BLE run simultaneously**

While the BLE scanner app is still connected and subscribed, load the
existing Web UI (`http://gaggia.local/`) in a browser on the same network
and confirm it still works normally (loads, live-updates, mode buttons
work) - this is the concrete check for spec §2's "WiFi and BLE run
simultaneously" requirement, and for this plan's "controlLoopTask timing is
undisturbed" constraint (watch for any new jitter/lag in the temperature
reading or SSR behavior with both transports active - there should be
none, since `controlLoopTask` is untouched by this plan).

- [ ] **Step 5: Record the outcome**

If all four checks pass, this plan is complete - update `AGENTS.md`'s
Change Log (newest entry at the top of §10, per the file's own header
instructions) noting the connectivity protocol is live over both BLE and
Serial, and that `AGENTS.md` §4 ("Files") should be extended to list the
five new source files this plan added
(`command_dispatch.cpp`/`telemetry.cpp`/`serial_transport.cpp`/
`ble_transport.cpp`/`connectivity.cpp`). No firmware commit needed for this
step - it's documentation only, do it directly rather than as a plan task.
