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
// wake, etc.) happens for this many minutes while BREW/STEAM is active, the
// heater is force-switched to OFF to save power. 0 = disabled. Configurable
// live from the Web UI, persisted in NVS.
// ============================================================================
#define ECO_TIMEOUT_MIN_DEFAULT 30

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
