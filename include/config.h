#pragma once

// ============================================================================
// Firmware build identifier
// ----------------------------------------------------------------------------
// __DATE__/__TIME__ are filled in by the compiler at build time, not by hand -
// unlike a manually-incremented version number, this can never go stale or
// get forgotten. Exists specifically so an OTA update's success can be
// confirmed from the Web UI/`/status` (compare this string before/after an
// upload) instead of guessing from HTTP response behavior alone.
// ============================================================================
#define FIRMWARE_BUILD_TIMESTAMP (__DATE__ " " __TIME__)

// ============================================================================
// Hardware watchdog - re-added 2026-08-23 (3rd attempt), via a DEDICATED
// FreeRTOS task instead of feeding from loop()
// ----------------------------------------------------------------------------
// First added 2026-08-18, pulled hours later after repeated hangs during OTA
// (WebServer's handleClient() blocks through the entire multipart upload
// without returning to loop(), so a watchdog pet only reachable from loop()
// starves mid-transfer). Tried again 2026-08-23 with the pet moved into the
// OTA upload handler itself - and it STILL panic-rebooted loopTask, at ~10s
// then ~25s across two boots, with NO OTA involved at all (plain WiFi
// connect + idle /status polling). That proved the root cause wasn't OTA
// specifically - it was sharing ANY task between safety-critical timing and
// anything network-related (WebServer, WiFiManager, MQTT reconnects can all
// block for seconds).
//
// Comparing against GaggiMate's architecture (AGENTS.md research,
// 2026-08-23) confirmed the fix: GaggiMate hit this exact failure mode
// (their own code comment describes a synchronous web-server task starving
// core 0 and tripping their watchdog) and solved it by moving brew-critical
// logic onto its OWN pinned FreeRTOS task, decoupled entirely from web/WiFi
// traffic - not by relocating where the watchdog is petted.
//
// This build does the same: main.cpp's controlLoopTask() runs the temp
// read/PID/safety-cutoff/shot-phase logic on its own task (pinned to core 1,
// CONTROL_TASK_PERIOD_MS cadence via vTaskDelayUntil for jitter-free timing),
// and is the ONLY task registered with the TWDT (esp_task_wdt_add(nullptr)
// inside the task itself). setup() calls disableLoopWDT() so the Arduino
// loop task - which now only runs MQTT networking - can never trip the
// watchdog no matter how long a broker reconnect or DNS lookup takes.
// Switching to ESPAsyncWebServer (replacing the synchronous WebServer) means
// OTA/HTTP handling never blocks any task the watchdog is watching either.
// If the control task itself ever hangs (a genuine bug, not a slow network
// op), it now has nowhere to hide.
// ============================================================================
#define CONTROL_TASK_PERIOD_MS 50
#define CONTROL_TASK_WDT_TIMEOUT_S 3
#define CONTROL_TASK_STACK_SIZE 4096

// ============================================================================
// Shared-state lock
// ----------------------------------------------------------------------------
// Splitting control logic onto its own task (above) means web.cpp's /update
// handler (runs on the AsyncTCP task) and mqtt.cpp's callback() (runs on the
// default Arduino loop task) now mutate PID/mode/shot globals CONCURRENTLY
// with controlTick() reading/writing them, instead of everything serializing
// through one task as before. main.cpp exposes lockState()/unlockState()
// (a recursive mutex - recursive because setOpMode() calls
// applyActiveProfile() internally, and callers hold the lock across both) so
// a settings save or MQTT command can never interleave with a half-finished
// control tick. controlTick(), the /update handler, and mqtt.cpp's
// callback() each hold it for their full body; /status and MQTT's
// publishStatus() take it only briefly to snapshot the fields they read.
// ============================================================================

// ============================================================================
// Sensor fault detection (2026-08-18, replaces a consecutive-bad-read latch)
// ----------------------------------------------------------------------------
// The previous rule ("fault only after N consecutive bad reads") is blind to
// a sensor that's intermittently flaky forever - e.g. failing 1-in-3 reads
// indefinitely never accumulates N consecutive failures, so it would never
// latch a fault, even though that's a genuinely unreliable sensor. Comparing
// against GaggiMate's own thermocouple fault handling (a rolling error-rate
// window) informed this replacement: track the last SENSOR_FAULT_WINDOW
// reads and latch a fault once the bad-rate within that window reaches
// SENSOR_FAULT_RATE_THRESHOLD, rather than requiring failures back-to-back.
// SENSOR_FAULT_MIN_SAMPLES keeps the same ~1s grace period the old rule gave
// a fresh boot/reconnect before evaluating the rate at all, so a handful of
// early reads can't trip a fault on a tiny sample (e.g. 1 bad out of 1 read
// is a 100% "rate" but meaningless).
// ============================================================================
#define SENSOR_FAULT_WINDOW 20
#define SENSOR_FAULT_MIN_SAMPLES 5
#define SENSOR_FAULT_RATE_THRESHOLD 0.5f

// ============================================================================
// Pin Definitions - ESP32-S3-DevKitC-1 (N16R8)
// ----------------------------------------------------------------------------
// Chosen to avoid reserved S3 pins:
//   - GPIO19/20  -> native USB (D-/D+)
//   - GPIO26-32  -> SPI flash
//   - GPIO33-37  -> Octal (OPI) PSRAM on R8 modules
//   - GPIO0/3/45/46 -> strapping pins
//   - GPIO48     -> onboard RGB LED
// ============================================================================
#define PIN_SSR 4

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

// ============================================================================
// Temperature sensor module (UART, NOT SPI)
// ----------------------------------------------------------------------------
// Despite its "MAX31865" silkscreen, this is an ASCII AT-command UART module
// (PT100 + relay board). Wiring: module TX -> PIN_SENSOR_RX, module RX ->
// PIN_SENSOR_TX, module 5V -> ESP 3V3, GND -> GND. Protocol and discovery
// process are documented in AGENTS.md Section 9 (Change Log) and in
// src/temp_sensor.cpp.
// ============================================================================
#define PIN_SENSOR_RX 18
#define PIN_SENSOR_TX 17
#define SENSOR_BAUD 9600

// ============================================================================
// Operating modes: OFF / BREW / STEAM
// ----------------------------------------------------------------------------
// Each non-OFF mode has its own setpoint + PID gains ("gain scheduling") and
// its own safety ceiling, since brew and steam are very different thermal
// regimes. The board always boots into OFF regardless of what was active
// before a reset/power-cycle - see setOpMode() in main.cpp.
//
// STEAM_* defaults below are PLACEHOLDERS, not verified against this specific
// machine's real thermal limits (open question: does it have an independent
// high-limit safety fuse, separate from the thermostats? see AGENTS.md
// Section 7 step 4). Test steam mode attended, briefly, until proven safe.
// ============================================================================
enum class OpMode { OFF, BREW, STEAM };

#define BREW_SETPOINT_DEFAULT 93.0
#define BREW_KP_DEFAULT 50.0
#define BREW_KI_DEFAULT 0.0
#define BREW_KD_DEFAULT 0.0
#define BREW_MAX_SAFETY 105.0

#define STEAM_SETPOINT_DEFAULT 123.0
#define STEAM_KP_DEFAULT 30.0
#define STEAM_KI_DEFAULT 0.0
#define STEAM_KD_DEFAULT 0.0

// ============================================================================
// Brew "active" gains + shot-start feedforward (2026-08-16)
// ----------------------------------------------------------------------------
// Real-hardware testing showed the gentle Brew gains above (tuned for a
// clean, non-overshooting idle heatup) are too weak to fight the much bigger,
// faster disturbance of actual brewing (cold water flowing through the
// boiler) - a shot sagged ~11-13C even with Ki added, because Ki is
// inherently slow to build up force and the shot is often over before it
// catches up. Researching Gaggiuino and GaggiMate's own solutions to this
// exact problem informed the fix here:
//   - Gaggiuino: no PID at all - explicit bang-bang thresholds, MUCH more
//     aggressive (full power sooner) while brewing than while idle.
//   - GaggiMate: keeps PID, but adds a physics-based feedforward term
//     (flow rate x specific heat x temp delta) applied the INSTANT flow is
//     detected, rather than waiting for a temperature error to develop.
// This project has no flow sensor yet (see AGENTS.md roadmap), so the
// feedforward here is a simple fixed boost instead of GaggiMate's flow-scaled
// one - the same "push immediately, don't wait for feedback" principle,
// without the sensor hardware their version depends on. Combined with a
// separate, more aggressive Brew gain profile (Gaggiuino's principle) used
// only while shotInProgress is true (main.cpp), reverting to the gentle
// profile the instant the shot ends.
// ============================================================================
// Revised 2026-08-16 after the first real-shot test: sag dropped from
// ~11-13C (gentle profile alone) to ~6C with the original 20/0.3/2 - a real
// improvement, but the user wants tighter still. Pushed further because the
// overshoot risk that constrains the GENTLE brew profile (a full climb from
// cold) simply doesn't apply here - this profile only ever runs while
// shotInProgress, which by construction starts already near brewSetpoint,
// not from cold. Free to be considerably more aggressive without
// reintroducing a heatup-overshoot problem.
#define BREW_ACTIVE_KP_DEFAULT 30.0
#define BREW_ACTIVE_KI_DEFAULT 0.5
#define BREW_ACTIVE_KD_DEFAULT 2.0

// Feedforward compensation (0-1000 output window, same scale as
// Output/WindowSize) added on top of the PID's own computed output every
// cycle while a shot is in progress and temperature sits below brewSetpoint
// - NOT a one-shot kick, a sustained boost for as long as there's a
// meaningful gap. Tapered smoothly to zero as currentTemperature approaches
// brewSetpoint (BREW_SHOT_FEEDFORWARD_TAPER_C is the width of that taper
// band) so it can't itself cause overshoot once the disturbance is already
// handled or the shot is ending - same safety-taper idea as GaggiMate's
// calculateSafetyScaling(), simplified since we lack their flow signal.
// Boost and taper width both increased 2026-08-16 alongside the gains above.
#define BREW_SHOT_FEEDFORWARD_BOOST 400.0
#define BREW_SHOT_FEEDFORWARD_TAPER_C 1.5

// Integral anti-windup (2026-08-16) - see the bleed logic in main.cpp's
// loop(). BAND is how close to setpoint counts as "arrived" (bleed fires
// once, letting the final approach run on P+D alone instead of a wound-up
// integral). REARM is how far error must drift back out before the next
// approach (e.g. recovering after a shot sag) can trigger another bleed -
// wider than BAND on purpose, so normal small wobbles right at setpoint
// don't repeatedly retrigger it.
#define INTEGRAL_BLEED_BAND_C 3.0
#define INTEGRAL_BLEED_REARM_C 6.0

// Steam's safety ceiling is configurable live from the Web UI (persisted in
// NVS) - unlike BREW_MAX_SAFETY, which stays a fixed constant. This default
// only applies if nothing's been saved yet. Clamped server-side to
// [STEAM_MAX_SAFETY_MIN, STEAM_MAX_SAFETY_MAX] to guard against a typo in
// the Web UI creating a dangerously high ceiling.
//
// Raised from 125 to 135 (2026-08-16): at the old value there was only 2C
// of headroom above STEAM_SETPOINT_DEFAULT (123), and real-hardware brew
// testing the same day showed 7-8C of overshoot was possible before the
// integral anti-windup fix (main.cpp) - steam's Ki was in the same range as
// brew's pre-fix idle profile, so the old margin was genuinely at risk of
// tripping on a normal heatup. 135 restores roughly the same proportional
// margin BREW_MAX_SAFETY gives brewSetpoint (~11-12C). Still a placeholder,
// not verified against this machine's real thermal limits - see AGENTS.md.
#define STEAM_MAX_SAFETY_DEFAULT 135.0
#define STEAM_MAX_SAFETY_MIN 100.0
#define STEAM_MAX_SAFETY_MAX 150.0

// Temperature smoothing: exponential moving average applied to each good
// sensor read (0 = no smoothing/always use raw, 1 = never update). Cuts
// sensor noise reaching the PID at the cost of a small lag.
#define TEMP_EMA_ALPHA 0.3f

// Temperature history ring buffer, sampled independently of the raw 250ms
// sensor read cadence, used by the Web UI sparkline.
#define TEMP_HISTORY_LEN 60
#define TEMP_HISTORY_SAMPLE_INTERVAL_MS 2000

// ============================================================================
// Eco / auto-sleep
// ----------------------------------------------------------------------------
// If no explicit user action (any /update call - mode change, tuning edit,
// wake, etc.) happens for this many minutes while BREW is active, the
// heater is force-switched to OFF to save power. 0 = disabled. Configurable
// live from the Web UI, persisted in NVS. STEAM uses its own, separate,
// much shorter timeout below - steaming is normally a brief task, and the
// steam boiler runs much hotter than brew, so leaving it idling for the
// same 30 minutes as Brew isn't the right default.
// ============================================================================
#define ECO_TIMEOUT_MIN_DEFAULT 30

// Steam-specific auto-off (2026-08-24) - same mechanism as the eco-sleep
// timer above (checkEcoSleep() in main.cpp), just a separate, independent
// timeout applied only while STEAM is the active mode. Switching into Steam
// itself counts as activity (any /update call does), so in practice this
// fires ~this-many-minutes after the last time the Web UI was touched while
// steaming - which is normally shortly after steaming actually finished.
// 0 = disabled, configurable live from the Web UI, persisted in NVS.
#define STEAM_AUTO_OFF_MIN_DEFAULT 2

// ============================================================================
// PID Autotune (relay feedback / Astrom-Hagglund method)
// ----------------------------------------------------------------------------
// Toggles the SSR between a fixed high and low output around the active
// profile's setpoint, measures the resulting oscillation period/amplitude,
// and derives Kp/Ki/Kd via the "no overshoot" Ziegler-Nichols relay-tuning
// variant (Kp=0.2Ku, Ki=0.4Ku/Pu, Kd=Ku*Pu/15 - see main.cpp) rather than the
// more aggressive classic formula. The very first half-cycle (the initial
// climb from whatever temperature autotune was started at) is discarded
// before averaging, since it isn't a real oscillation and its arbitrary
// starting-temperature-dependent "amplitude" was confirmed to skew Ku wildly
// between runs (2026-08-16). Respects the existing activeMaxSafety ceiling
// and sensor-fault handling at all times - aborts immediately if either
// trips. Supervise every run.
//
// Convergence, not a fixed cycle count (2026-08-16): a relay-feedback test
// only produces a trustworthy Ku/Pu once the induced oscillation has settled
// into a genuine, repeatable limit cycle - blindly averaging exactly N
// cycles risks including ones that are still transitioning toward that limit
// cycle, especially right after the (now-discarded) warm-up climb. Instead,
// the last AUTOTUNE_CONVERGE_CYCLES cycle amplitudes are kept in a small
// ring buffer; once they all agree with each other within
// AUTOTUNE_CONVERGE_TOLERANCE (relative to their mean), the oscillation is
// considered settled and Ku/Pu are computed from that window. If it never
// quite converges, AUTOTUNE_MAX_CYCLES is a hard cap that finalizes anyway
// using the best available window, rather than running indefinitely -
// AUTOTUNE_MAX_RUNTIME_MS remains the ultimate time-based backstop under
// all of this.
// ============================================================================
#define AUTOTUNE_RELAY_HIGH 500.0     // "high" Output level (0-1000 window) - not full power
#define AUTOTUNE_RELAY_LOW 0.0        // "low" Output level
#define AUTOTUNE_HYSTERESIS_C 0.5     // deg C band around setpoint before switching relay state
#define AUTOTUNE_CONVERGE_CYCLES 3    // trailing window size, both for the convergence check and the final Ku/Pu average
#define AUTOTUNE_CONVERGE_TOLERANCE 0.15 // max relative spread (vs. mean) allowed across the window to call it "settled"
#define AUTOTUNE_MAX_CYCLES 10        // hard cap - finalize with the best available window even if never fully converged
#define AUTOTUNE_MAX_RUNTIME_MS (30UL * 60UL * 1000UL) // hard abort ceiling regardless of progress

enum class AutotuneState { IDLE, RUNNING, DONE_OK, DONE_FAIL };

// ============================================================================
// Shot timer + history log
// ----------------------------------------------------------------------------
// No hardware yet detects the physical brew button/pump (see AGENTS.md
// roadmap - a current-sensor or a pump-control relay would enable automatic
// detection later). Until then, shots are started/stopped manually from the
// Web UI. Each record includes a `weight` field, populated 0 (unmeasured)
// until a BLE scale exists - avoids a schema change when that's added.
// Stored as an append-only CSV on LittleFS (the same "spiffs" partition
// already defined in platformio.ini's min_spiffs.csv), not NVS - NVS is a
// flat key-value store, poorly suited to a growing log.
// ============================================================================
#define SHOT_LOG_PATH "/shots.csv"
#define SHOT_LOG_MAX_ENTRIES 200 // oldest trimmed once exceeded

// Auto-stop: once a shot has been running this many seconds, main.cpp's
// loop() calls stopShot() on its own - no manual "Stop Shot" tap required.
// 0 = disabled (manual stop only), configurable live from the Web UI,
// persisted in NVS. Default sits in the middle of the 25-30s SCA-referenced
// extraction window already shown in the Web UI.
//
// IMPORTANT - this is a software-only auto-stop right now: it ends the
// firmware's own shot bookkeeping (timer, history log entry, reverting the
// Brew gain profile from active back to gentle - see the brew-active gains
// above) - it does NOT physically stop the pump. There's no hardware yet
// that can cut the pump's own power (see AGENTS.md/HARDWARE_ROADMAP.md item
// 4, not yet built) - water keeps flowing until the machine's own Brew
// switch is released by hand, same as always. Don't confuse "the shot timer
// stopped" with "the machine stopped brewing" until item 4 exists.
#define SHOT_AUTO_STOP_SEC_DEFAULT 27
#define SHOT_AUTO_STOP_SEC_MIN 5
#define SHOT_AUTO_STOP_SEC_MAX 90

// ============================================================================
// Descale / maintenance reminder
// ----------------------------------------------------------------------------
// Both thresholds configurable live from the Web UI, persisted in NVS.
// ============================================================================
#define DESCALE_SHOT_THRESHOLD_DEFAULT 100
#define DESCALE_DAY_THRESHOLD_DEFAULT 60

// ============================================================================
// Shot profiles (2026-08-16, second redesign)
// ----------------------------------------------------------------------------
// Superseded the previous "Espresso=default + Ristretto/Lungo offsets"
// design the same day, after deciding to fold in pre-infusion: a real named-
// profile LIST (like GaggiMate's file-per-profile approach, scaled down for
// a single-user home machine rather than an open-ended library) solves the
// original "can't unpress" complaint more naturally than the offset hack did
// - there's no "off" state to escape, you just tap a DIFFERENT complete
// profile, and the original default is always sitting right there in the
// list. Stored as one CSV line per profile on LittleFS (profiles.cpp),
// mirroring shot_log.cpp's existing pattern rather than introducing a JSON
// library - each profile is small (name + a handful of numbers).
//
// PROFILE_MAX_COUNT=8 deliberately bounded (not Gaggiuino's cramped 5-slot
// EEPROM array, not GaggiMate's unbounded file-per-profile design meant for
// a shareable library) - a single home user realistically wants a handful of
// named recipes (a couple of beans/roasts, ristretto/lungo variants), not a
// browsable catalog.
// ============================================================================
#define PROFILE_MAX_COUNT 8
#define PROFILE_NAME_MAX_LEN 20
#define PROFILE_LOG_PATH "/profiles.csv"

// Pre-infusion (2026-08-16) - see AGENTS.md/HARDWARE_ROADMAP.md item 4 for
// the full reasoning. Pulses the pump relay on/off a few times before
// switching to continuous power for the rest of the shot, approximating the
// puck-saturation benefit of true low-pressure pre-infusion using only an
// on/off relay (no dimmer/pressure transducer needed - those remain a
// separate, harder item). Per-profile, not global - each saved profile
// carries its own pre-infusion pattern (or none). Has no effect on the real
// pump until the mains-side splice (item 4) is done; the phase state machine
// and timing were built ahead of the hardware, same "software ahead of
// hardware" pattern already proven for shot auto-stop.
#define PREINFUSION_PULSES_DEFAULT 4
#define PREINFUSION_PULSES_MAX 10
#define PREINFUSION_ON_MS_DEFAULT 1000
#define PREINFUSION_OFF_MS_DEFAULT 1000
#define PREINFUSION_PULSE_MS_MIN 200
#define PREINFUSION_PULSE_MS_MAX 5000

enum class ShotPhase { NONE, PREINFUSION_ON, PREINFUSION_OFF, PRESSURE, EXTRACTION };

// ============================================================================
// Scheduled warm-up (2026-08-16; multiple slots added same day)
// ----------------------------------------------------------------------------
// Auto-switches to Brew or Steam at a configured local time of day, once per
// slot per calendar day, using the NTP-synced UTC clock already set up for
// shot log timestamps (see setup()/configTime in main.cpp). SCHED_MAX_COUNT
// fixed slots (not a fully dynamic list, same bounded-slots approach as the
// brew presets) - independently enabled, so e.g. a weekday morning slot and
// a separate weekend-morning slot can coexist without fighting each other.
// One shared timezone offset for all slots (not per-slot) - auto-detected
// from the browser (see syncSchedTz() in web.cpp), not entered by hand; a
// wrong manual UTC offset was the original bug that made the schedule look
// like it "didn't work" (it fired, just hours off from the intended local
// time). Each slot independently guarded against firing before NTP has
// synced (an unsynced clock reads as 1970) and against re-firing more than
// once per calendar day via its own "day index" (days since epoch, local
// time) that resets on rollover.
// ============================================================================
#define SCHED_MAX_COUNT 3
#define SCHED_ENABLED_DEFAULT false
#define SCHED_HOUR_DEFAULT 7
#define SCHED_MIN_DEFAULT 0
#define SCHED_TZ_OFFSET_MIN_DEFAULT 0
// SCHED_MODE_DEFAULT: false = Brew, true = Steam (stored as a bool - only
// two useful choices, matches OpMode without pulling in a 3rd NVS type)
#define SCHED_MODE_STEAM_DEFAULT false
