#include "config.h"
#include "dimmer.h"
#include "mqtt.h"
#include "pressure_sensor.h"
#include "profiles.h"
#include "shot_log.h"
#include "temp_sensor.h"
#include "web.h"
#include <Arduino.h>
#include <PID_v1.h>
#include <Preferences.h>
#include <time.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// Coarse-grained lock protecting every global the control loop reads/writes
// (currentTemperature, Setpoint/Output, currentMode, shotInProgress,
// currentShotPhase, tempHistory, brew/steam gains, activeMaxSafety) - see
// config.h "Shared-state lock" for why this exists now that control logic
// runs on its own FreeRTOS task (controlLoopTask() below) instead of inline
// in loop(). Recursive: setOpMode() calls applyActiveProfile() internally,
// and callers hold the lock across both.
static SemaphoreHandle_t stateMutex = nullptr;
void lockState() { xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY); }
void unlockState() { xSemaphoreGiveRecursive(stateMutex); }

// Global Variables
float currentTemperature = 0.0;
bool sensorFault = false;
unsigned long lastTempReadTime = 0;
const unsigned long TEMP_READ_INTERVAL = 250; // Read every 250ms

// Temperature history (for the Web UI sparkline), sampled independently of
// the raw read cadence above.
float tempHistory[TEMP_HISTORY_LEN] = {0};
int tempHistoryHead = 0;
int tempHistoryCount = 0;
unsigned long lastHistorySample = 0;

// PID Variables
double Setpoint, Input, Output;
// PID Constants - mirror of whichever profile (brew/steam) is currently
// active; the profiles below are the source of truth.
double Kp, Ki, Kd;
PID myPID(&Input, &Output, &Setpoint, Kp, Ki, Kd, DIRECT);

// SSR Control Variables
int WindowSize = 1000; // 1000ms window for SSR
unsigned long windowStartTime;

// Operating mode + per-profile tuning (persisted in NVS in setup()/web.cpp).
// Always boots OFF - see setOpMode() below.
OpMode currentMode = OpMode::OFF;
double brewSetpoint = BREW_SETPOINT_DEFAULT, brewKp = BREW_KP_DEFAULT,
       brewKi = BREW_KI_DEFAULT, brewKd = BREW_KD_DEFAULT;
// Separate, more aggressive Brew gains used only while a shot is actively in
// progress - see config.h for the full reasoning (Gaggiuino/GaggiMate
// research, 2026-08-16). Declared here (not down with the rest of the shot-
// timer state) so applyActiveProfile() below can read shotInProgress.
bool shotInProgress = false;
double brewActiveKp = BREW_ACTIVE_KP_DEFAULT, brewActiveKi = BREW_ACTIVE_KI_DEFAULT,
       brewActiveKd = BREW_ACTIVE_KD_DEFAULT;
double steamSetpoint = STEAM_SETPOINT_DEFAULT, steamKp = STEAM_KP_DEFAULT,
       steamKi = STEAM_KI_DEFAULT, steamKd = STEAM_KD_DEFAULT;
double steamMaxSafety = STEAM_MAX_SAFETY_DEFAULT; // configurable, unlike BREW_MAX_SAFETY
double activeMaxSafety = BREW_MAX_SAFETY;

// Integral anti-windup state (see the bleed logic in loop()) - starts armed
// so the very first approach to setpoint after boot also gets bled.
bool integralBleedArmed = true;

// Applies the active profile's setpoint/gains to the live PID. Used both by
// setOpMode() (mode switch) and by the Web UI when the currently-active
// profile's own values are edited live (no mode change, so no PID reset).
// Also called the instant a shot starts/stops (see startShot()/stopShot())
// so the Brew gain switch takes effect immediately, not on the next
// unrelated Web UI action.
static void applyActiveProfile() {
  if (currentMode == OpMode::BREW) {
    Setpoint = brewSetpoint;
    if (shotInProgress) {
      Kp = brewActiveKp;
      Ki = brewActiveKi;
      Kd = brewActiveKd;
    } else {
      Kp = brewKp;
      Ki = brewKi;
      Kd = brewKd;
    }
    activeMaxSafety = BREW_MAX_SAFETY;
  } else if (currentMode == OpMode::STEAM) {
    Setpoint = steamSetpoint;
    Kp = steamKp;
    Ki = steamKi;
    Kd = steamKd;
    activeMaxSafety = steamMaxSafety;
  }
  myPID.SetTunings(Kp, Ki, Kd);
}

void refreshActiveProfileIfChanged() {
  if (currentMode != OpMode::OFF) applyActiveProfile();
}

void setOpMode(OpMode newMode) {
  currentMode = newMode;

  if (newMode == OpMode::OFF) {
    myPID.SetMode(MANUAL);
    Output = 0;
    return;
  }

  applyActiveProfile();

  // Bumpless PID reset: cycling MANUAL->AUTOMATIC makes PID_v1 re-seed its
  // internal accumulator from the current Output/Input rather than carry
  // over whatever the PREVIOUS profile's regime left behind. Matters most
  // for a big setpoint jump like brew->steam.
  myPID.SetMode(MANUAL);
  Output = 0;
  myPID.SetMode(AUTOMATIC);
}

// ============================================================================
// Eco / auto-sleep
// ----------------------------------------------------------------------------
// lastActivityTime advances only on explicit user actions (any /update call
// from the Web UI - mode change, tuning edit, wake) - NOT on passive /status
// polling, so a browser tab left open silently doesn't prevent sleep.
// Brew and Steam use independent timeouts (see config.h) - Steam's is much
// shorter by default, since steaming is normally brief and the steam
// boiler runs hotter than Brew's.
// ============================================================================
unsigned long lastActivityTime = 0;
unsigned long ecoTimeoutMin = ECO_TIMEOUT_MIN_DEFAULT; // 0 = disabled, persisted
unsigned long steamAutoOffMin = STEAM_AUTO_OFF_MIN_DEFAULT; // 0 = disabled, persisted
bool autoSleeping = false;
OpMode modeBeforeSleep = OpMode::OFF;

void noteActivity() { lastActivityTime = millis(); }

static void checkEcoSleep(unsigned long now) {
  if (currentMode == OpMode::OFF) return; // nothing to sleep
  if (autoSleeping) return;               // already asleep

  OpMode modeNow = currentMode; // capture before setOpMode() below changes it
  unsigned long timeoutMin = (modeNow == OpMode::STEAM) ? steamAutoOffMin : ecoTimeoutMin;
  if (timeoutMin == 0) return; // disabled for this mode

  unsigned long timeoutMs = timeoutMin * 60000UL;
  if (now - lastActivityTime >= timeoutMs) {
    modeBeforeSleep = modeNow;
    autoSleeping = true;
    setOpMode(OpMode::OFF);
    if (modeNow == OpMode::STEAM) {
      Serial.println("Steam auto-off: no Web UI activity - Steam mode -> Off");
      Serial.println("Steam auto-off: heater forced OFF");
    } else {
      Serial.println("Eco sleep: no Web UI activity, heater forced OFF");
    }
  }
}

void wakeFromSleep() {
  if (!autoSleeping) return;
  autoSleeping = false;
  setOpMode(modeBeforeSleep);
  noteActivity();
}

// ============================================================================
// PID Autotune (relay feedback / Astrom-Hagglund method)
// ----------------------------------------------------------------------------
// While RUNNING, this OWNS `Output` directly (PID is in MANUAL) - toggles it
// between a fixed high/low around the target profile's setpoint, discards
// the initial non-oscillating warm-up cycle, then keeps measuring full
// cycles into a small trailing window until that window's amplitudes agree
// with each other (convergence) or a hard cycle cap is hit - see config.h
// for the full reasoning and finishAutotune()/autotuneHasConverged() below
// for the implementation. Derives Kp/Ki/Kd from the converged window via the
// "no overshoot" Ziegler-Nichols relay formula. The existing safety-ceiling
// force-off check in loop() (activeMaxSafety / sensor fault) still applies
// unconditionally underneath this - autotune adds its own abort checks on
// top rather than replacing that layer.
// ============================================================================
AutotuneState autotuneState = AutotuneState::IDLE;
String autotuneMessage = "";

static OpMode autotuneForMode = OpMode::BREW;
static double autotuneTargetTemp = 0;
static bool autotuneRelayHigh = false;
static double autotuneMinTemp = 0, autotuneMaxTemp = 0;
static unsigned long autotuneLastSwitchTime = 0;
static unsigned long autotuneStartTime = 0;
// The first HIGH->LOW transition spans the initial climb from whatever
// temperature autotune was started at up to the target, not a real
// oscillation - its "amplitude" is just (target - starting temp)/2, unrelated
// to the boiler's actual relay-feedback dynamics. Discarding it prevents that
// arbitrary starting-temperature-dependent value from skewing the Ku/Pu
// average (confirmed on real hardware: Ku varied 58.86 vs 208.31 between two
// runs started at different temperatures, 2026-08-16).
static bool autotuneWarmupDiscarded = false;

// Trailing ring buffer of the last AUTOTUNE_CONVERGE_CYCLES good cycles -
// see config.h for why convergence, not a fixed count, decides when enough
// data has been gathered. autotuneCycleCount counts every good cycle ever
// seen (used only to gate "have we got at least a full window yet" and "have
// we hit the hard cap"); the buffers themselves always hold just the most
// recent window, oldest overwritten first.
static double autotuneRecentAmplitudes[AUTOTUNE_CONVERGE_CYCLES];
static double autotuneRecentPeriodsMs[AUTOTUNE_CONVERGE_CYCLES];
static int autotuneRecentIndex = 0;
static int autotuneCycleCount = 0;

void startAutotune(OpMode forMode) {
  if (autotuneState == AutotuneState::RUNNING) return;
  if (currentMode == OpMode::OFF) {
    autotuneState = AutotuneState::DONE_FAIL;
    autotuneMessage = "Cannot start: select Brew or Steam mode first";
    return;
  }
  if (sensorFault || currentTemperature <= 0) {
    autotuneState = AutotuneState::DONE_FAIL;
    autotuneMessage = "Cannot start: sensor fault or no reading yet";
    return;
  }
  autoSleeping = false; // don't let eco-sleep fight the autotune
  autotuneForMode = forMode;
  autotuneTargetTemp = (forMode == OpMode::BREW) ? brewSetpoint : steamSetpoint;
  autotuneRelayHigh = true;
  autotuneMinTemp = currentTemperature;
  autotuneMaxTemp = currentTemperature;
  autotuneCycleCount = 0;
  autotuneRecentIndex = 0;
  autotuneWarmupDiscarded = false;
  autotuneLastSwitchTime = millis();
  autotuneStartTime = millis();
  autotuneState = AutotuneState::RUNNING;
  autotuneMessage = "Running";
  myPID.SetMode(MANUAL); // Output is driven directly below, not by PID.Compute()
  Serial.printf("Autotune: started for %s, target %.1fC\n",
                forMode == OpMode::BREW ? "brew" : "steam", autotuneTargetTemp);
}

static void abortAutotune(const char *reason) {
  Serial.print("Autotune: aborted - ");
  Serial.println(reason);
  autotuneState = AutotuneState::DONE_FAIL;
  autotuneMessage = String("Aborted: ") + reason;
  Output = 0;
  setOpMode(currentMode); // restores normal PID control on the active profile
}

// Public entry point for a user-requested stop (Web UI "Stop" button, or a
// mode change while autotune is running - see the /update handler in
// web.cpp, which calls this BEFORE applying the new mode so a mode-button
// click always safely interrupts an in-progress autotune).
void stopAutotune() {
  if (autotuneState != AutotuneState::RUNNING) return;
  abortAutotune("stopped by user");
}

// True once the last AUTOTUNE_CONVERGE_CYCLES measured amplitudes agree with
// each other within AUTOTUNE_CONVERGE_TOLERANCE of their mean - i.e. the
// relay-induced oscillation has settled into a genuine, repeatable limit
// cycle rather than still drifting toward one. Checked on amplitude
// specifically, not period: Ku = 4d/(pi*amplitude) is inversely proportional
// to amplitude, so amplitude drift is exactly what previously let Ku swing
// 3.5x between runs (2026-08-16) - period is comparatively stable and adds
// little extra signal here.
static bool autotuneHasConverged() {
  double sum = 0;
  double minV = autotuneRecentAmplitudes[0];
  double maxV = autotuneRecentAmplitudes[0];
  for (int i = 0; i < AUTOTUNE_CONVERGE_CYCLES; i++) {
    double v = autotuneRecentAmplitudes[i];
    sum += v;
    if (v < minV) minV = v;
    if (v > maxV) maxV = v;
  }
  double mean = sum / AUTOTUNE_CONVERGE_CYCLES;
  if (mean <= 0.0) return false; // degenerate reading, not real convergence
  return (maxV - minV) / mean <= AUTOTUNE_CONVERGE_TOLERANCE;
}

// Averages the trailing window into Ku/Pu, derives Kp/Ki/Kd via the "no
// overshoot" Ziegler-Nichols relay formula, applies + persists them to
// whichever profile (brew/steam) was under test, and resumes normal control.
// `converged` distinguishes a clean finish from one forced by hitting
// AUTOTUNE_MAX_CYCLES without ever settling within tolerance - surfaced in
// the result message so a forced-but-noisy result isn't mistaken for a
// fully-settled one.
static void finishAutotune(bool converged) {
  double amplitudeSum = 0, periodSum = 0;
  for (int i = 0; i < AUTOTUNE_CONVERGE_CYCLES; i++) {
    amplitudeSum += autotuneRecentAmplitudes[i];
    periodSum += autotuneRecentPeriodsMs[i];
  }
  double avgAmplitude = amplitudeSum / AUTOTUNE_CONVERGE_CYCLES;
  double avgPeriodMs = periodSum / AUTOTUNE_CONVERGE_CYCLES;

  if (avgAmplitude < 0.1) {
    abortAutotune("oscillation too small to measure - target too close to ambient?");
    return;
  }

  double Pu = avgPeriodMs / 1000.0;                            // ultimate period, seconds
  double d = (AUTOTUNE_RELAY_HIGH - AUTOTUNE_RELAY_LOW) / 2.0;  // relay half-amplitude
  double Ku = (4.0 * d) / (PI * avgAmplitude);                  // ultimate gain

  // "No overshoot" Ziegler-Nichols relay-tuning variant (Kp=0.2Ku, Ti=Pu/2,
  // Td=Pu/3) - see config.h for why this replaced the classic formula.
  // Kp/Ki/Kd here are in the same per-second units SetTunings() already
  // expects (PID_v1 rescales internally by its own sample time) - matches
  // how the Web UI's manual tuning fields have worked all along.
  double newKp = 0.2 * Ku;
  double newKi = 0.4 * Ku / Pu;
  double newKd = Ku * Pu / 15.0; // 0.2*Ku * Pu/3

  if (autotuneForMode == OpMode::BREW) {
    brewKp = newKp;
    brewKi = newKi;
    brewKd = newKd;
  } else {
    steamKp = newKp;
    steamKi = newKi;
    steamKd = newKd;
  }
  Preferences preferences;
  preferences.begin("gaggia", false);
  if (autotuneForMode == OpMode::BREW) {
    preferences.putDouble("brew_kp", brewKp);
    preferences.putDouble("brew_ki", brewKi);
    preferences.putDouble("brew_kd", brewKd);
  } else {
    preferences.putDouble("steam_kp", steamKp);
    preferences.putDouble("steam_ki", steamKi);
    preferences.putDouble("steam_kd", steamKd);
  }
  preferences.end();

  autotuneState = AutotuneState::DONE_OK;
  char msg[112];
  snprintf(msg, sizeof(msg),
           "Done%s: Ku=%.2f Pu=%.1fs -> Kp=%.2f Ki=%.4f Kd=%.2f",
           converged ? "" : " (forced, didn't fully converge)", Ku, Pu, newKp,
           newKi, newKd);
  autotuneMessage = msg;
  Serial.println(String("Autotune: ") + msg);

  Output = 0;
  setOpMode(currentMode); // resume normal PID control with the new gains
}

static void runAutotuneStep(unsigned long now) {
  // Same safety ceiling as normal operation, checked independently here since
  // PID.Compute() (which normally enforces it via Input/Output limits) is
  // bypassed while autotune drives Output directly.
  if (sensorFault || currentTemperature <= 0) {
    abortAutotune("sensor fault");
    return;
  }
  if (currentTemperature > activeMaxSafety) {
    abortAutotune("over safety ceiling");
    return;
  }
  if (now - autotuneStartTime > AUTOTUNE_MAX_RUNTIME_MS) {
    abortAutotune("max runtime exceeded");
    return;
  }

  if (currentTemperature < autotuneMinTemp) autotuneMinTemp = currentTemperature;
  if (currentTemperature > autotuneMaxTemp) autotuneMaxTemp = currentTemperature;

  // Relay switching with hysteresis around the target temperature.
  if (autotuneRelayHigh &&
      currentTemperature >= autotuneTargetTemp + AUTOTUNE_HYSTERESIS_C) {
    autotuneRelayHigh = false;
    unsigned long halfPeriod = now - autotuneLastSwitchTime;
    autotuneLastSwitchTime = now;

    if (!autotuneWarmupDiscarded) {
      // Discard this one - see autotuneWarmupDiscarded's declaration comment.
      autotuneWarmupDiscarded = true;
      Serial.println("Autotune: discarding warm-up cycle (initial climb to target)");
    } else {
      double amplitude = (autotuneMaxTemp - autotuneMinTemp) / 2.0;
      double periodMs = halfPeriod * 2.0; // approximate full period

      autotuneRecentAmplitudes[autotuneRecentIndex] = amplitude;
      autotuneRecentPeriodsMs[autotuneRecentIndex] = periodMs;
      autotuneRecentIndex = (autotuneRecentIndex + 1) % AUTOTUNE_CONVERGE_CYCLES;
      autotuneCycleCount++;

      Serial.printf("Autotune: cycle %d - amplitude %.2fC, period %.1fs\n",
                    autotuneCycleCount, amplitude, periodMs / 1000.0);

      bool haveFullWindow = autotuneCycleCount >= AUTOTUNE_CONVERGE_CYCLES;
      bool converged = haveFullWindow && autotuneHasConverged();
      bool hitMaxCycles = autotuneCycleCount >= AUTOTUNE_MAX_CYCLES;

      if (converged) {
        finishAutotune(true);
        return;
      }
      if (hitMaxCycles) {
        Serial.println("Autotune: hit AUTOTUNE_MAX_CYCLES without converging - finishing with best available window");
        finishAutotune(false);
        return;
      }
    }
    autotuneMinTemp = currentTemperature;
    autotuneMaxTemp = currentTemperature;
  } else if (!autotuneRelayHigh &&
             currentTemperature <= autotuneTargetTemp - AUTOTUNE_HYSTERESIS_C) {
    autotuneRelayHigh = true;
    autotuneLastSwitchTime = now;
  }

  Output = autotuneRelayHigh ? AUTOTUNE_RELAY_HIGH : AUTOTUNE_RELAY_LOW;
}

// ============================================================================
// Shot timer + history log
// ----------------------------------------------------------------------------
// Started manually from the Web UI (Start Shot button) - the ESP32 has no
// visibility into the machine's own Brew switch/pump yet, so it can't detect
// a shot beginning on its own (see HARDWARE_ROADMAP.md for the sensor that
// would). Stopping can now happen automatically via shotAutoStopSec (below),
// timed from that manual start - see the loop() check further down and its
// important caveat: this stops the firmware's own bookkeeping, not the pump
// itself (no hardware yet to do that either).
// (shotInProgress itself is declared up with the other PID globals, not
// here - applyActiveProfile() needs it for Brew gain-scheduling.)
// ============================================================================
unsigned long shotStartMillis = 0;
float shotPeakTemp = 0.0;
unsigned long shotAutoStopSec = SHOT_AUTO_STOP_SEC_DEFAULT; // 0 = disabled, persisted

// Descale / maintenance reminder - both persisted in NVS.
unsigned long shotCount = 0;         // shots since the last descale/reset
time_t lastDescaleTime = 0;          // epoch seconds; 0 = never set (unknown)
unsigned long descaleShotThreshold = DESCALE_SHOT_THRESHOLD_DEFAULT;
unsigned long descaleDayThreshold = DESCALE_DAY_THRESHOLD_DEFAULT;

// ============================================================================
// Shot profiles (see config.h/profiles.cpp) - a saved profile is just a
// named (temp, auto-stop, pre-infusion pattern) bundle you can load into the
// plain live settings below. Loading one does NOT bind future edits to it -
// editing "Brew target" directly still works exactly as before; it just
// means the live value may no longer match whichever profile was last
// loaded, same as how the live settings always worked before profiles
// existed. activeProfileIndex is remembered purely so the Web UI can
// highlight "which one did I last load," not as a strict live binding.
// ============================================================================
int activeProfileIndex = 0; // persisted; -1 = none loaded / edited since
bool activePreinfusionEnabled = false;
int activePreinfusionPulses = 0;
int activePreinfusionOnMs = 0;
int activePreinfusionOffMs = 0;

// Loads profile `idx` into the live settings (brewSetpoint/shotAutoStopSec/
// pre-infusion pattern), persists them exactly like editing those fields by
// hand would, and remembers idx as the active profile for UI highlighting.
void applyProfile(int idx) {
  String name;
  double temp;
  unsigned long autoStop;
  bool preinfEnabled;
  int pulses, onMs, offMs;
  if (!profileGet(idx, name, temp, autoStop, preinfEnabled, pulses, onMs, offMs)) return;

  activeProfileIndex = idx;
  brewSetpoint = temp;
  shotAutoStopSec = autoStop;
  activePreinfusionEnabled = preinfEnabled;
  activePreinfusionPulses = pulses;
  activePreinfusionOnMs = onMs;
  activePreinfusionOffMs = offMs;

  Preferences preferences;
  preferences.begin("gaggia", false);
  preferences.putInt("active_profile", activeProfileIndex);
  preferences.putDouble("brew_target", brewSetpoint);
  preferences.putULong("shot_auto_stop", shotAutoStopSec);
  preferences.putBool("pi_enabled", activePreinfusionEnabled);
  preferences.putInt("pi_pulses", activePreinfusionPulses);
  preferences.putInt("pi_on_ms", activePreinfusionOnMs);
  preferences.putInt("pi_off_ms", activePreinfusionOffMs);
  preferences.end();

  refreshActiveProfileIfChanged();
}

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
static float plainDutyPercent = 0.0f;

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
    plainDutyPercent = (stage.type == ShotStage::Type::PUMP_OFF) ? 0.0f : 100.0f;
    dimmerSetPowerPercent(plainDutyPercent);
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

void stopShot() {
  if (!shotInProgress) return;
  shotInProgress = false;
  pressureClosedLoopActive = false;
  pressurePID.SetMode(MANUAL);
  dimmerSetPowerPercent(0.0f); // force the pump off, same intent as the old relay's stopShot()
  currentShotPhase = ShotPhase::NONE;
  activeStageCount = 0;
  currentStageIndex = 0;
  unsigned long durationMs = millis() - shotStartMillis;
  // 0 = unknown, same convention as weight - covers both a genuine fault
  // (-999) and the plain "no reading yet" default (0) at the moment of stop.
  float endTemp = (currentTemperature > 0) ? currentTemperature : 0.0f;
  shotLogAppend(time(nullptr), durationMs, shotPeakTemp, 0.0, endTemp);

  // Bumpless reset back to the gentle profile: the aggressive active-brewing
  // Ki can accumulate a real amount of integral over a 25-30s shot, and
  // carrying that into the much smaller gentle-profile Ki would linger and
  // risk exactly the kind of post-disturbance overshoot this project has
  // already seen from windup elsewhere. Cycling MANUAL->AUTOMATIC re-seeds
  // PID_v1's internal accumulator fresh, same trick setOpMode() already uses
  // for mode switches.
  if (currentMode == OpMode::BREW) {
    myPID.SetMode(MANUAL);
    Output = 0;
    refreshActiveProfileIfChanged();
    myPID.SetMode(AUTOMATIC);
  }

  shotCount++;
  Preferences preferences;
  preferences.begin("gaggia", false);
  preferences.putULong("shot_count", shotCount);
  preferences.end();
}

void markDescaled() {
  shotCount = 0;
  lastDescaleTime = time(nullptr);
  Preferences preferences;
  preferences.begin("gaggia", false);
  preferences.putULong("shot_count", shotCount);
  preferences.putULong("last_descale", (unsigned long)lastDescaleTime);
  preferences.end();
}

// ============================================================================
// Scheduled warm-up (see config.h) - SCHED_MAX_COUNT independent slots, each
// firing setOpMode() once per calendar day (local time, via the one shared
// schedTzOffsetMin) at its own configured hour:minute. Checked every loop()
// tick in main.cpp's loop(), guarded per-slot against firing before NTP has
// synced and against re-firing within the same day.
// ============================================================================
bool schedEnabled[SCHED_MAX_COUNT] = {SCHED_ENABLED_DEFAULT, false, false};
int schedHour[SCHED_MAX_COUNT] = {SCHED_HOUR_DEFAULT, SCHED_HOUR_DEFAULT, SCHED_HOUR_DEFAULT};
int schedMin[SCHED_MAX_COUNT] = {SCHED_MIN_DEFAULT, SCHED_MIN_DEFAULT, SCHED_MIN_DEFAULT};
bool schedModeSteam[SCHED_MAX_COUNT] = {SCHED_MODE_STEAM_DEFAULT, SCHED_MODE_STEAM_DEFAULT, SCHED_MODE_STEAM_DEFAULT};
int schedTzOffsetMin = SCHED_TZ_OFFSET_MIN_DEFAULT; // shared - see config.h
static long schedFiredDayIndex[SCHED_MAX_COUNT] = {-1, -1, -1}; // -1 = never fired

static void checkScheduledWarmup(time_t utcNow) {
  if (utcNow < 1600000000L) return; // NTP not synced yet (reads as ~1970 otherwise)

  time_t localNow = utcNow + (time_t)schedTzOffsetMin * 60;
  struct tm lt;
  gmtime_r(&localNow, &lt); // gmtime, not localtime - offset already applied above
  long dayIndex = (long)(localNow / 86400L);

  for (int i = 0; i < SCHED_MAX_COUNT; i++) {
    if (!schedEnabled[i]) continue;
    if (dayIndex != schedFiredDayIndex[i] &&
        lt.tm_hour == schedHour[i] && lt.tm_min == schedMin[i]) {
      Serial.printf("Scheduled warm-up %d: switching to %s\n", i,
                    schedModeSteam[i] ? "steam" : "brew");
      setOpMode(schedModeSteam[i] ? OpMode::STEAM : OpMode::BREW);
      schedFiredDayIndex[i] = dayIndex;
    }
  }
}

// Re-arms slot `i` so it can fire again today - called from web.cpp whenever
// that slot's enabled/time is edited. Without this, firing once and then
// adjusting the time later the same day (e.g. testing, or just changing your
// mind about tomorrow's wake-up time) would silently refuse to fire again
// until the NEXT calendar day - confirmed 2026-08-16 as the actual cause of
// a schedule that appeared to "stop working" after an earlier edit that day.
void resetSchedFired(int i) {
  if (i >= 0 && i < SCHED_MAX_COUNT) schedFiredDayIndex[i] = -1;
}

static void controlLoopTask(void *pv); // defined below setup(), used by it

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Gaggia PID Controller Starting...");

  stateMutex = xSemaphoreCreateRecursiveMutex();

  // Exclude the Arduino loop task (MQTT networking only, from here on) from
  // the TWDT entirely - see config.h "Hardware watchdog" for why this is the
  // fix for the two prior reverted attempts, not just relocating the pet.
  disableLoopWDT();

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

  // Initialize temperature sensor (UART PT100 module)
  Serial.println("Initializing temperature sensor...");
  tempSensorInit();

  // Initialize Preferences (NVS) - load both profiles
  Preferences preferences;
  preferences.begin("gaggia", true); // true = read-only
  brewSetpoint = preferences.getDouble("brew_target", BREW_SETPOINT_DEFAULT);
  brewKp = preferences.getDouble("brew_kp", BREW_KP_DEFAULT);
  brewKi = preferences.getDouble("brew_ki", BREW_KI_DEFAULT);
  brewKd = preferences.getDouble("brew_kd", BREW_KD_DEFAULT);
  brewActiveKp = preferences.getDouble("brew_akp", BREW_ACTIVE_KP_DEFAULT);
  brewActiveKi = preferences.getDouble("brew_aki", BREW_ACTIVE_KI_DEFAULT);
  brewActiveKd = preferences.getDouble("brew_akd", BREW_ACTIVE_KD_DEFAULT);
  steamSetpoint = preferences.getDouble("steam_target", STEAM_SETPOINT_DEFAULT);
  steamKp = preferences.getDouble("steam_kp", STEAM_KP_DEFAULT);
  steamKi = preferences.getDouble("steam_ki", STEAM_KI_DEFAULT);
  steamKd = preferences.getDouble("steam_kd", STEAM_KD_DEFAULT);
  steamMaxSafety = preferences.getDouble("steam_max_safety", STEAM_MAX_SAFETY_DEFAULT);
  ecoTimeoutMin = preferences.getULong("eco_min", ECO_TIMEOUT_MIN_DEFAULT);
  steamAutoOffMin = preferences.getULong("steam_off_min", STEAM_AUTO_OFF_MIN_DEFAULT);
  shotAutoStopSec = preferences.getULong("shot_auto_stop", SHOT_AUTO_STOP_SEC_DEFAULT);
  shotCount = preferences.getULong("shot_count", 0);
  lastDescaleTime = (time_t)preferences.getULong("last_descale", 0);
  descaleShotThreshold =
      preferences.getULong("descale_shots", DESCALE_SHOT_THRESHOLD_DEFAULT);
  descaleDayThreshold =
      preferences.getULong("descale_days", DESCALE_DAY_THRESHOLD_DEFAULT);
  activeProfileIndex = preferences.getInt("active_profile", 0);
  activePreinfusionEnabled = preferences.getBool("pi_enabled", false);
  activePreinfusionPulses = preferences.getInt("pi_pulses", PREINFUSION_PULSES_DEFAULT);
  activePreinfusionOnMs = preferences.getInt("pi_on_ms", PREINFUSION_ON_MS_DEFAULT);
  activePreinfusionOffMs = preferences.getInt("pi_off_ms", PREINFUSION_OFF_MS_DEFAULT);
  for (int i = 0; i < SCHED_MAX_COUNT; i++) {
    String p = "sched" + String(i) + "_";
    schedEnabled[i] = preferences.getBool((p + "en").c_str(), i == 0 ? SCHED_ENABLED_DEFAULT : false);
    schedHour[i] = preferences.getInt((p + "hr").c_str(), SCHED_HOUR_DEFAULT);
    schedMin[i] = preferences.getInt((p + "mn").c_str(), SCHED_MIN_DEFAULT);
    schedModeSteam[i] = preferences.getBool((p + "st").c_str(), SCHED_MODE_STEAM_DEFAULT);
  }
  schedTzOffsetMin = preferences.getInt("sched_tz_min", SCHED_TZ_OFFSET_MIN_DEFAULT);
  preferences.end();

  // Shot history log + named shot profiles (both LittleFS)
  shotLogInit();
  profilesInit();

  // NTP time sync, for real shot timestamps. Stored/compared in UTC
  // throughout - the Web UI converts to local time for display.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  Serial.printf("Loaded brew: target=%.1f Kp=%.1f Ki=%.1f Kd=%.1f\n",
                brewSetpoint, brewKp, brewKi, brewKd);
  Serial.printf("Loaded steam: target=%.1f Kp=%.1f Ki=%.1f Kd=%.1f maxSafety=%.1f\n",
                steamSetpoint, steamKp, steamKi, steamKd, steamMaxSafety);
  Serial.printf("Eco sleep timeout: %lu min (0 = disabled)\n", ecoTimeoutMin);

  // Initialize PID - always boots OFF regardless of what was active before a
  // reset/power-cycle (safety: never auto-resume heating unattended).
  windowStartTime = millis();
  myPID.SetOutputLimits(0, WindowSize);
  setOpMode(OpMode::OFF);
  noteActivity();

  // Initialize Web Interface
  setupWeb();

  // Initialize MQTT
  setupMQTT();

  // Hardware watchdog, scoped to controlLoopTask only (see config.h). The
  // Arduino core auto-initializes its own TWDT before setup() runs, so
  // esp_task_wdt_init() here fails with ESP_ERR_INVALID_STATE - fall back to
  // reconfiguring the existing one rather than treating that as an error.
  esp_task_wdt_config_t wdtConfig = {
      .timeout_ms = CONTROL_TASK_WDT_TIMEOUT_S * 1000,
      .idle_core_mask = 0, // only the control task is watched, not idle tasks
      .trigger_panic = true,
  };
  esp_err_t wdtErr = esp_task_wdt_init(&wdtConfig);
  if (wdtErr == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&wdtConfig);
  }

  // Pinned to core 1, away from the WiFi/AsyncTCP stack's usual core 0 work,
  // same core-separation principle GaggiMate uses for its brew-logic task.
  xTaskCreatePinnedToCore(controlLoopTask, "control", CONTROL_TASK_STACK_SIZE,
                           nullptr, configMAX_PRIORITIES - 2, nullptr, 1);
}

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

// Runs on its own FreeRTOS task (controlLoopTask() below), not inline in
// loop() - see config.h "Hardware watchdog" for why. Holds stateMutex for
// its entire body so web.cpp's /update handler and mqtt.cpp's callback()
// (each running on a different task) can never observe or cause a
// half-updated mix of mode/setpoint/gains/shot-phase state.
static void controlTick(unsigned long now) {
  lockState();

  // Read Temperature
  if (now - lastTempReadTime >= TEMP_READ_INTERVAL) {
    lastTempReadTime = now;
    float temp;
    TempSensorStatus status = tempSensorRead(temp);

    // Fault check - rolling error-rate window (see config.h for why this
    // replaced a consecutive-bad-read latch). badWindow[] holds the last
    // SENSOR_FAULT_WINDOW read outcomes (true = bad); badCount is kept in
    // sync incrementally rather than resummed every read.
    static bool badWindow[SENSOR_FAULT_WINDOW] = {false};
    static int badWindowIndex = 0, badWindowFilled = 0, badCount = 0;

    bool badRead = (status != TempSensorStatus::OK);
    bool newFault = updateFaultWindow(badWindow, SENSOR_FAULT_WINDOW, badWindowIndex,
                                       badWindowFilled, badCount, SENSOR_FAULT_MIN_SAMPLES,
                                       SENSOR_FAULT_RATE_THRESHOLD, badRead);

    if (badRead) {
      Serial.println(status == TempSensorStatus::ERROR_REPLY
                          ? "Sensor replied ERROR_1"
                          : "Sensor read timed out");
    }

    if (newFault && !sensorFault) {
      Serial.printf("Sensor fault latched: %d/%d bad reads in window\n", badCount,
                    badWindowFilled);
    } else if (!newFault && sensorFault) {
      Serial.println("Sensor fault cleared: read rate recovered");
    }
    sensorFault = newFault;

    // Keyed off the AGGREGATE fault state, not just this instant's read
    // result - while sensorFault is latched, don't trust currentTemperature
    // even if this particular read happened to succeed (a stray good read
    // mixed into an otherwise-bad window shouldn't let the PID branch treat
    // the reading as valid again before the fault actually clears).
    if (sensorFault) {
      currentTemperature = -999.0; // Real error
    } else if (badRead) {
      // This read failed, but the overall rate is still below the fault
      // threshold - keep using the last valid temperature, don't blend a
      // bad reading in.
    } else {
      // Exponential moving average - smooths sensor noise reaching the PID.
      // Skip blending right after boot or a fault (no meaningful history).
      if (currentTemperature <= 0 || currentTemperature == -999.0) {
        currentTemperature = temp;
      } else {
        currentTemperature =
            TEMP_EMA_ALPHA * temp + (1.0f - TEMP_EMA_ALPHA) * currentTemperature;
      }

      // Safety Cutoff (mode-aware: brew and steam have different ceilings) -
      // logging only here; actual enforcement is the PID-branch gate and the
      // SSR force-off check further down in loop(), both keyed off the same
      // activeMaxSafety.
      if (currentTemperature > activeMaxSafety) {
        Serial.println("SAFETY CUTOFF TRIGGERED");
      }

      if (shotInProgress && currentTemperature > shotPeakTemp) {
        shotPeakTemp = currentTemperature;
      }
    }
  }

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

  // Temperature history sampling (independent of the 250ms read cadence) -
  // skip while in a hard fault so the sparkline doesn't get poisoned by -999.
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

  // Shot auto-stop - see config.h/SHOT_AUTO_STOP_SEC_DEFAULT for the full
  // caveat: this ends the firmware's OWN shot bookkeeping (timer, history
  // log, Brew gain profile revert) at the configured duration, timed from
  // the manual Start Shot tap. It does not physically stop the pump - no
  // hardware exists yet to do that (see HARDWARE_ROADMAP.md item 4).
  if (shotInProgress && shotAutoStopSec > 0 &&
      now - shotStartMillis >= shotAutoStopSec * 1000UL) {
    Serial.printf("Shot auto-stop: %lu s reached\n", shotAutoStopSec);
    stopShot();
  }

  // Shot stage state machine - unthrottled (pulse timing can be sub-second),
  // but a cheap no-op once in EXTRACTION or with no shot running (see
  // tickShotStages()).
  tickShotStages(now);

  // Pump restore - only while a shot is actually running (shotInProgress);
  // stopShot() already forced the dimmer to 0% and nothing should touch it
  // again until the next startShot(), otherwise the plain-duty branch below
  // would immediately re-drive the dimmer back to its last recorded duty
  // (typically 100%) on the very next tick after stop. While a shot IS
  // running: pressure-targeting stages (pressureClosedLoopActive, set by
  // applyShotStagePumpOutput() above, including any transition
  // tickShotStages() just made this tick) get a fresh PID compute every
  // tick; plain-duty stages get plainDutyPercent (set at their own stage
  // transition) re-asserted every tick too, so the safety-ceiling check
  // right below has something correct to restore once a trip clears.
  if (shotInProgress) {
    if (pressureClosedLoopActive) {
      pressureInput = currentPressure;
      pressurePID.Compute();
      dimmerSetPowerPercent((float)pressureOutput);
    } else {
      dimmerSetPowerPercent(plainDutyPercent);
    }
  }

  // Safety ceiling always wins, independent of PID/profile output - same
  // rule as activeMaxSafety for temperature.
  if (pressureFault || currentPressure > PUMP_MAX_SAFETY_BAR) {
    dimmerSetPowerPercent(0.0f);
  }

  // Scheduled warm-up (see config.h) - minute-granularity, so checking once a
  // second is more than enough and avoids a redundant time()/gmtime_r() call
  // on every single loop() iteration.
  static unsigned long lastSchedCheck = 0;
  if (now - lastSchedCheck >= 1000) {
    lastSchedCheck = now;
    checkScheduledWarmup(time(nullptr));
  }

  // PID Computation - autotune (if running) owns Output directly instead.
  // currentMode==OFF is checked FIRST and unconditionally: activeMaxSafety
  // stays at whatever the last active profile left it, so the temperature
  // condition below is true almost all the time, Off included - without
  // this explicit gate, the integral-bleed/safety-reset code's bumpless
  // MANUAL->AUTOMATIC toggle could silently re-engage the PID (and start
  // firing the heater on the last-active setpoint/gains) while the UI still
  // shows Off. setOpMode(OFF) already parked things in MANUAL with
  // Output=0; nothing below should ever undo that while Off.
  if (currentMode == OpMode::OFF) {
    // no-op
  } else if (autotuneState == AutotuneState::RUNNING) {
    runAutotuneStep(now);
  } else if (currentTemperature > 0 && currentTemperature < activeMaxSafety) {
    // Integral anti-windup (2026-08-16): a long, far-from-setpoint approach
    // (cold boiler warmup, or recovering after a big brew-time sag) lets
    // PID_v1's internal integral accumulator build up a large "energy
    // commitment" that can't unwind fast enough once setpoint is reached -
    // confirmed on real hardware climbing to a 94C brew target: output was
    // STILL ~35% (350-363/1000) two-plus degrees PAST setpoint, and thermal
    // lag then carried the reading to 101C+ before it turned around. The
    // accumulator is private to the library, so the only way to clear it is
    // the same bumpless MANUAL->AUTOMATIC trick already used elsewhere
    // (PID::Initialize() reseeds it from the just-zeroed Output). Bled once
    // per approach, the first time error crosses within
    // INTEGRAL_BLEED_BAND_C of setpoint, so the final stretch runs on
    // P(+D) alone instead of dragging that stale commitment through the
    // crossing. Re-arms once error drifts back out past
    // INTEGRAL_BLEED_REARM_C, so it also catches the climb back to
    // setpoint after a shot sag, not just the initial cold start.
    double errorNow = Setpoint - currentTemperature;
    if (errorNow > INTEGRAL_BLEED_REARM_C) {
      integralBleedArmed = true;
    } else if (integralBleedArmed && errorNow <= INTEGRAL_BLEED_BAND_C) {
      Output = 0;
      myPID.SetMode(MANUAL);
      myPID.SetMode(AUTOMATIC);
      integralBleedArmed = false;
      Serial.println("Integral bleed: PID reset on setpoint approach");
    }

    Input = currentTemperature;
    myPID.Compute();

    // TEMPORARY diagnostic override (2026-08-17): force full heater output
    // for the whole shot, replacing the additive feedforward this used to
    // be (see git history). The additive form - PID output + up to
    // BREW_SHOT_FEEDFORWARD_BOOST, capped at WindowSize - was assumed to
    // reliably saturate at max during a shot, but real-hardware observation
    // the same day showed it often doesn't: whether it reaches the cap
    // depends on the PID's own live integral state, which the integral-
    // bleed fix now actively limits. This forces TRUE 100% duty the whole
    // time brewSetpoint hasn't been reached, to directly test whether that
    // closes the brew-time sag any further. Existing safety systems
    // (activeMaxSafety/BREW_MAX_SAFETY cutoff just above, and the SSR
    // force-off check further down in loop()) remain fully independent of
    // this and still apply - this cannot exceed the safety ceiling. Revisit
    // once real shot data (History tab's peak/end temp) shows whether this
    // actually helps.
    if (currentMode == OpMode::BREW && shotInProgress &&
        autotuneState != AutotuneState::RUNNING &&
        currentTemperature < brewSetpoint) {
      Output = (double)WindowSize;
    }
  } else {
    // Forced OFF - sensor fault or over the safety ceiling. Also reset the
    // PID's integral accumulator (same bumpless trick as above) so it isn't
    // left wound-up at whatever value it held right before crossing the
    // ceiling. Previously this only happened when the mode was manually
    // cycled Off->Brew, which is exactly why the temperature looked like it
    // "wouldn't come back down" on its own after tripping the safety cutoff
    // - the stale accumulator could re-fire the heater at high duty the
    // instant it dipped back under the ceiling.
    Output = 0;
    myPID.SetMode(MANUAL);
    myPID.SetMode(AUTOMATIC);
    integralBleedArmed = true; // re-arm so the next approach gets a fresh bleed too
  }

  // Eco / auto-sleep (never fights an in-progress autotune)
  if (autotuneState != AutotuneState::RUNNING) {
    checkEcoSleep(now);
  }

  // SSR Control (Time Proportioning)
  if (now - windowStartTime > WindowSize) {
    windowStartTime += WindowSize;
  }

  // Safety check just in case
  if (currentTemperature > activeMaxSafety || currentTemperature == -999.0) {
    digitalWrite(PIN_SSR, LOW);
  } else {
    if (Output > now - windowStartTime)
      digitalWrite(PIN_SSR, HIGH);
    else
      digitalWrite(PIN_SSR, LOW);
  }

  unlockState();
}

// Dedicated task so temp read/PID/safety-cutoff/shot-phase timing can never
// be starved by WiFi/web/MQTT work on other tasks - see config.h "Hardware
// watchdog". Registers itself (only itself) with the TWDT and pets it once
// per tick; vTaskDelayUntil keeps the period jitter-free regardless of how
// long the tick body itself took.
static void controlLoopTask(void *pv) {
  esp_task_wdt_add(nullptr);
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    controlTick(millis());
    esp_task_wdt_reset();
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(CONTROL_TASK_PERIOD_MS));
  }
}

// Everything safety/timing-relevant now runs on controlLoopTask - this task
// only ever does MQTT networking, which can block for seconds on a slow
// broker/DNS without risking the watchdog (disableLoopWDT() in setup()
// excludes this task from the TWDT entirely).
void loop() {
  handleMQTT();
  delay(10);
}
