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

// Steam's safety ceiling is configurable live from the Web UI (persisted in
// NVS) - unlike BREW_MAX_SAFETY, which stays a fixed constant. This default
// only applies if nothing's been saved yet. Clamped server-side to
// [STEAM_MAX_SAFETY_MIN, STEAM_MAX_SAFETY_MAX] to guard against a typo in
// the Web UI creating a dangerously high ceiling.
#define STEAM_MAX_SAFETY_DEFAULT 125.0
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

// ============================================================================
// Descale / maintenance reminder
// ----------------------------------------------------------------------------
// Both thresholds configurable live from the Web UI, persisted in NVS.
// ============================================================================
#define DESCALE_SHOT_THRESHOLD_DEFAULT 100
#define DESCALE_DAY_THRESHOLD_DEFAULT 60
