# Water Level Sensor (Item 6): Design Spec

**Status:** approved by the user in chat, 2026-10-15 (UI mockups reviewed:
"tank OK" and "low water" states). **Implemented** on
`feature/water-level-sensor` (based on `wip/local`); built in CI, not yet
flashed or bench-tested. Implementation notes are in Section 7.
**Blocked on:** the firmware actually running on the machine
(`build Oct 2 2026 12:47:26`) has local changes that are on no GitHub
branch: a "Water (est.)" stat tile and a pump calibration field
(`input_pump_cal_grams`). Implementation has to start from that code, not
from `main`, or flashing it would silently remove features. The user is
pushing it to `wip/local`.
**Scope:** firmware (`src/`, `include/`), the embedded Web UI
(`src/web.cpp`), MQTT/Home Assistant, docs.

## 1. Goal

Warn when the water tank is low, and stop a new shot from starting on an
empty tank (pump running dry). The sensor only answers "water at this
height: yes or no". It isn't a level gauge.

Non-goals:
- Measuring volume. There's one switch point, at the sensor's mounting
  height.
- Turning the heater off on low water. A low tank doesn't empty the
  boiler, and keeping it hot means the machine is ready as soon as it's
  refilled.
- Stopping a shot that's already running. The sensor sits at a "low"
  mark, not at empty, so there's still reserve; cutting the pump mid-shot
  ruins the coffee for no safety gain.

## 2. Hardware (summary; full detail in `HARDWARE_ROADMAP.md` item 6)

- Non-contact capacitive liquid level switch, 3.3-5V, ~1.5 mA, push-pull
  output. Mounted on the outside of the tank wall at the low-water height.
- Red → 3V3, black → GND, yellow → **GPIO7**.
- Output: **LOW = water present, HIGH = no water** (datasheet). A
  removed tank reads as "no water", which is the right answer for an
  interlock.
- 3.3V is the sensor's minimum supply and the board's rail measures
  3.26V, so it needs a bench test before install (fallback: 5V from the
  USB charger plus a divider on OUT).

## 3. Backend (firmware)

### 3.1 New module: `include/water_level.h` / `src/water_level.cpp`

Same shape as `pressure_sensor.*`/`temp_sensor.*`: a small module with no
web or MQTT knowledge.

```cpp
enum class WaterState : uint8_t { DISABLED, OK, LOW };

void waterLevelInit();                 // pinMode(PIN_WATER_LEVEL, INPUT_PULLUP)
void waterLevelTick(unsigned long now);// sample + debounce, called from controlTick()
WaterState waterLevelState();          // debounced state
bool waterLevelRawHigh();              // last raw read, for the Settings live view
```

- **Sampling:** read the pin every `WATER_SAMPLE_INTERVAL_MS` (100 ms)
  inside `controlTick()` on the control task, the same place every other
  sensor is read. `digitalRead()` is cheap and ISR-free.
- **Debounce:** the state only changes after `WATER_DEBOUNCE_SAMPLES`
  (20 samples = 2 s) of consistent raw reads in a row. Water sloshing
  when the pump starts, or reseating the tank, mustn't flicker the state
  or block a shot at random. The asymmetry is deliberate and stays simple:
  one counter, reset on any disagreeing read.
- **Polarity:** `waterActiveLow` (persisted, default `true`, per the
  datasheet) maps raw HIGH/LOW to water/no water.
- **Disabled:** when `waterSensorEnabled == false` (the default until the
  sensor is installed), the state is `DISABLED`, nothing is blocked, and
  GPIO7 is still configured with its pull-up so a floating pin is harmless.
- **Boot:** the state starts as `OK`, not `LOW`, and only becomes `LOW`
  after a full debounce window, so a reboot never shows a false alarm.

### 3.2 `include/config.h`

```cpp
#define PIN_WATER_LEVEL 7
#define WATER_SAMPLE_INTERVAL_MS 100
#define WATER_DEBOUNCE_SAMPLES 20         // 2 s of consistent reads
#define WATER_SENSOR_ENABLED_DEFAULT false
#define WATER_ACTIVE_LOW_DEFAULT true     // datasheet: LOW = water present
#define WATER_INTERLOCK_DEFAULT true      // block shot start on low water
```

GPIO7 is free, not a strapping/flash/PSRAM/USB pin, and sits next to
GPIO6 on the DevKitC-1 J1 header.

### 3.3 Settings (NVS, namespace `gaggia`)

| Key | Type | Default | Meaning |
|---|---|---|---|
| `water_en` | bool | `false` | Sensor installed / in use |
| `water_low_act` | bool | `true` | Raw LOW means water present |
| `water_lock` | bool | `true` | Block shot start when LOW |

Loaded in `setup()` with the other preferences, written from `/update`.

### 3.4 Shot interlock

In `startShot()` (`main.cpp`), the single entry point for every shot
start, so the Web UI and any future MQTT/button path get it for free:

```cpp
if (waterSensorEnabled && waterInterlock && waterLevelState() == WaterState::LOW) {
  shotBlockedReason = ShotBlock::LOW_WATER;
  shotBlockedAtMillis = millis();
  Serial.println("Shot blocked: low water");
  return;
}
```

- A running shot is never stopped by the sensor (Section 1).
- `shot_blocked` is reported in `/status` for ~5 s after a refused start,
  so the UI can explain why the button didn't start a shot, instead of
  the optimistic timer just silently resetting.

### 3.5 `/status` additions

```json
"water_enabled": true,
"water_state": "ok",          // "ok" | "low" | "disabled"
"water_raw_high": false,      // live pin level, for calibration
"water_active_low": true,
"water_interlock": true,
"shot_blocked": "low_water"   // or "" - only for ~5 s after a refusal
```

Snapshotted under `stateMutex` like every other field in `handleStatus()`.
Existing field names are untouched (MQTT/HA and the UI rely on them).

### 3.6 `/update` parameters

`water_enabled=0|1`, `water_active_low=0|1`, `water_interlock=0|1`. Each
one is persisted to NVS. `/update` already counts as user activity for the
eco timer, so there's nothing new to handle there.

### 3.7 MQTT / Home Assistant

- `gaggia/status` gains `"water_state"` (same values as `/status`).
- New HA discovery entity: `binary_sensor` `water_low`,
  `device_class: problem`, `value_template` on `water_state == 'low'`,
  attached to the same device and availability topic as the existing
  entities. It's only published when the sensor is enabled, so HA doesn't
  show a permanently "OK" sensor that doesn't exist.

### 3.8 Removed: "Water (est.)"

Per the user, the existing "Water (est.)" stat tile and the code behind it
are removed (tile, computation, `/status` fields). The pump calibration
field (`input_pump_cal_grams`) is removed too **if** it only feeds that
estimate. To confirm when reading the `wip/local` code.

## 4. Frontend (embedded Web UI in `src/web.cpp`)

Constraints from the 2026-09-15 redesign spec still apply: one embedded
string, no external assets, Android 4.4 tablet Firefox as the baseline
(no `inset` shorthand; keep syntax conservative).

### 4.1 Now tab

| # | Element | Tank OK | Low water |
|---|---|---|---|
| 1 | Header chip (next to the mode pill) | green "Tank OK" + droplet icon | amber "Low water" + droplet-off icon |
| 2 | Banner area | nothing | amber banner: "Refill the water tank. New shots are paused until it's back." Not dismissible; clears itself when water is detected |
| 3 | Shot button | "Start Shot" (unchanged) | amber "Refill to brew". Still tappable: a tap shows "Tank is low. Refill, then tap again." under it and doesn't start the timer |
| 4 | Stat tiles | Heater · Pump · **Tank (OK)** | Heater · Pump · **Tank (Low)** in amber |

- Amber, not red: red stays reserved for safety faults (sensor fault,
  pressure ceiling).
- When the sensor is disabled, the chip and the Tank tile are hidden
  completely, so the UI looks exactly as it does today.
- A running shot keeps its normal Stop button even if the tank goes low
  mid-shot; only the banner and chip change.
- `toggleShot()` stops starting the timer optimistically when the UI
  already knows the tank is low; if the device refuses anyway
  (`shot_blocked`), the UI rolls the timer back and shows the message.

### 4.2 Small fixes bundled with this work (seen on the live screen)

5. **Stuck sleep banner:** "Steam off · Heater off — idle timeout" kept
   showing after a manual Off. Fixed on the backend by the
   `setOpMode()`/`autoSleeping` fix (PR #24). The UI also hides the banner
   whenever the user changes mode.
6. **"Target --°C" when Off** → shows "Heater off".
7. **"Ns ago" in red** → red only when the mode is Brew/Steam or the
   controller is actually unreachable, not when it's idle and Off.

### 4.3 Settings tab → Power → new card "Water tank sensor"

- **Sensor installed:** toggle (`water_enabled`)
- **Water reads as:** select, "LOW (default)" / "HIGH" (`water_active_low`)
- **Pause shots on low water:** toggle (`water_interlock`)
- **Live reading · GPIO7:** e.g. "LOW · water detected" / "HIGH · no
  water", updated every `/status` poll (2 s) from `water_raw_high` +
  polarity, so the user can calibrate after install (full / empty / tank
  out) without touching code.
- Hint text: where to mount it, and that a removed tank reads as no water.

Saving goes through the existing intercepted `/update` form handler
(inline "Saved" confirmation, no page reload).

## 5. Test plan

Bench (USB only, mains off, per AGENTS.md §6):
1. Sensor disabled: GPIO7 floating, UI identical to today, shots start.
2. Enable, tank full: live reading LOW, chip "Tank OK", shots start.
3. Drain below the sensor: after ~2 s, "Low water" chip, banner, tile and
   button; Start refused with the message; `/status.shot_blocked` set.
4. Refill: back to OK within ~2 s, no reload needed.
5. Tank removed: reads as low water.
6. Polarity flip in Settings inverts the result live.
7. Shot running, then drain: the shot finishes; the next start is refused.
8. Reboot with the tank full: no false "Low water" at boot.
9. MQTT on: `water_state` in `gaggia/status`; the HA `binary_sensor`
   appears only when enabled.
10. Android 4.4 tablet: Now and Settings render and work.

CI: `.github/workflows/build.yml` must pass before any OTA. Check
`shot_in_progress` is false before flashing.

## 6. Rollout

1. User pushes the local firmware to `wip/local`.
2. Rebase PR #24 (auto-off fix, CI, docs) onto it.
3. Implement Sections 3-4 on that branch; CI build.
4. OTA via `/firmware`, sensor still disabled → verify nothing changed.
5. Install the sensor, enable it in Settings, run the bench steps above.

## 7. Implementation notes (2026-10-15)

- Built on `wip/local` (the code actually running on the machine), not
  `main`.
- The enum value is `WaterState::LOW_WATER`, not `LOW`: `LOW` is an
  Arduino macro.
- The interlock also refuses a **pump calibration run** on low water (it
  runs the pump at 100% for 10 s), through the same `waterBlocksPumpStart()`.
- A refused shot start returns **409 + reason** from `/update` (the existing
  refusal path used by calibration/profiles), and the UI rolls back its
  optimistic timer and shows the reason. `shot_blocked` is in `/status` too.
- **"Water (est.)" removed from the UI and from `/status` (`ml_est`)** only.
  The planner's internal `mlEst` and the **pump calibration stay**: on
  `wip/local` the calibration (ml/stroke) drives the whole pump model,
  the estimated bar and the phase profiles, not just that tile.
- Settings toggles save on change (`setWater()` → `/update`), not through
  the generic form handler, which would send a checkbox as "on" either way.
- Fix 7 (red "Ns ago") was **dropped**: it only turns red after 6 s with no
  `/status` reply, which is a real connection loss (the offline banner shows
  at the same moment). The "51s ago" seen live came from a background tab
  whose polling the browser had throttled.
- The HA `binary_sensor` `water_low` is added/removed on the fly
  (`mqttWaterDiscoveryDirty`) when the sensor is switched on/off in Settings.
- Checked here: the embedded JS passes `node --check`, and the Now and
  Settings screens were rendered in Chromium against a mock `/status` (OK
  and low-water states, plus the refused Start tap).
