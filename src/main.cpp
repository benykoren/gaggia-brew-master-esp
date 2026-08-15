#include "config.h"
#include "mqtt.h"
#include "shot_log.h"
#include "temp_sensor.h"
#include "web.h"
#include <Arduino.h>
#include <PID_v1.h>
#include <Preferences.h>
#include <time.h>

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
// ============================================================================
unsigned long lastActivityTime = 0;
unsigned long ecoTimeoutMin = ECO_TIMEOUT_MIN_DEFAULT; // 0 = disabled, persisted
bool autoSleeping = false;
OpMode modeBeforeSleep = OpMode::OFF;

void noteActivity() { lastActivityTime = millis(); }

static void checkEcoSleep(unsigned long now) {
  if (ecoTimeoutMin == 0) return;         // disabled
  if (currentMode == OpMode::OFF) return; // nothing to sleep
  if (autoSleeping) return;               // already asleep
  unsigned long timeoutMs = ecoTimeoutMin * 60000UL;
  if (now - lastActivityTime >= timeoutMs) {
    modeBeforeSleep = currentMode;
    autoSleeping = true;
    setOpMode(OpMode::OFF);
    Serial.println("Eco sleep: no Web UI activity, heater forced OFF");
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
}

void stopShot() {
  if (!shotInProgress) return;
  shotInProgress = false;
  unsigned long durationMs = millis() - shotStartMillis;
  shotLogAppend(time(nullptr), durationMs, shotPeakTemp, 0.0);

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

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Gaggia PID Controller Starting...");

  // Initialize/Configure Pins
  pinMode(PIN_SSR, OUTPUT);
  digitalWrite(PIN_SSR, LOW);

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
  shotAutoStopSec = preferences.getULong("shot_auto_stop", SHOT_AUTO_STOP_SEC_DEFAULT);
  shotCount = preferences.getULong("shot_count", 0);
  lastDescaleTime = (time_t)preferences.getULong("last_descale", 0);
  descaleShotThreshold =
      preferences.getULong("descale_shots", DESCALE_SHOT_THRESHOLD_DEFAULT);
  descaleDayThreshold =
      preferences.getULong("descale_days", DESCALE_DAY_THRESHOLD_DEFAULT);
  preferences.end();

  // Shot history log (LittleFS)
  shotLogInit();

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
}

void loop() {
  unsigned long now = millis();

  // MQTT Handling
  handleMQTT();

  // Read Temperature
  if (now - lastTempReadTime >= TEMP_READ_INTERVAL) {
    lastTempReadTime = now;
    float temp;
    TempSensorStatus status = tempSensorRead(temp);

    // Fault Check. Single shared counter so good reads actually reset it
    // (previously two separate block-scoped statics meant the counter never
    // reset on a good read and would latch a false -999 error over time).
    static uint8_t faultCounter = 0;
    if (status != TempSensorStatus::OK) {
      Serial.println(status == TempSensorStatus::ERROR_REPLY
                          ? "Sensor replied ERROR_1"
                          : "Sensor read timed out");

      faultCounter++;

      // Only report error if fault persists for > 1 second (4 readings)
      if (faultCounter > 4) {
        currentTemperature = -999.0; // Real error
        sensorFault = true;
      } else {
        // Transient fault - keep using last valid temperature
        Serial.println("Transient Sensor Fault - Ignoring");
      }
    } else {
      faultCounter = 0; // Reset counter on good read
      sensorFault = false;

      // Exponential moving average - smooths sensor noise reaching the PID.
      // Skip blending right after boot or a fault (no meaningful history).
      if (currentTemperature <= 0 || currentTemperature == -999.0) {
        currentTemperature = temp;
      } else {
        currentTemperature =
            TEMP_EMA_ALPHA * temp + (1.0f - TEMP_EMA_ALPHA) * currentTemperature;
      }

      // Safety Cutoff (mode-aware: brew and steam have different ceilings)
      if (currentTemperature > activeMaxSafety) {
        Serial.println("SAFETY CUTOFF TRIGGERED");
         // Additional safety logic could go here
      }

      if (shotInProgress && currentTemperature > shotPeakTemp) {
        shotPeakTemp = currentTemperature;
      }
    }
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

  // PID Computation - autotune (if running) owns Output directly instead
  if (autotuneState == AutotuneState::RUNNING) {
    runAutotuneStep(now);
  } else if (currentTemperature > 0 && currentTemperature < activeMaxSafety) {
    Input = currentTemperature;
    myPID.Compute();

    // Shot-start feedforward (see config.h) - an open-loop boost added on
    // top of the PID's own output, timed to the KNOWN start of the
    // disturbance rather than waiting for an error to develop. Tapered to
    // zero as currentTemperature approaches/exceeds brewSetpoint so it can't
    // itself drive an overshoot once the disturbance is already handled.
    if (currentMode == OpMode::BREW && shotInProgress &&
        autotuneState != AutotuneState::RUNNING) {
      double margin = brewSetpoint - currentTemperature; // >0 while still below target
      double taper = constrain(margin / BREW_SHOT_FEEDFORWARD_TAPER_C, 0.0, 1.0);
      Output = constrain(Output + BREW_SHOT_FEEDFORWARD_BOOST * taper, 0.0,
                          (double)WindowSize);
    }
  } else {
    Output = 0; // Forced OFF if error or over temp
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

  // Handle Web Server Requests
  handleWebLoop();
}
