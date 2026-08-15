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
double steamSetpoint = STEAM_SETPOINT_DEFAULT, steamKp = STEAM_KP_DEFAULT,
       steamKi = STEAM_KI_DEFAULT, steamKd = STEAM_KD_DEFAULT;
double steamMaxSafety = STEAM_MAX_SAFETY_DEFAULT; // configurable, unlike BREW_MAX_SAFETY
double activeMaxSafety = BREW_MAX_SAFETY;

// Applies the active profile's setpoint/gains to the live PID. Used both by
// setOpMode() (mode switch) and by the Web UI when the currently-active
// profile's own values are edited live (no mode change, so no PID reset).
static void applyActiveProfile() {
  if (currentMode == OpMode::BREW) {
    Setpoint = brewSetpoint;
    Kp = brewKp;
    Ki = brewKi;
    Kd = brewKd;
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
// between a fixed high/low around the target profile's setpoint, measures
// the resulting oscillation period + amplitude, and derives Kp/Ki/Kd via
// classic Ziegler-Nichols relay-tuning formulas. The existing safety-ceiling
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
static int autotuneCycleCount = 0;
static double autotunePeriodSumMs = 0;
static double autotuneAmplitudeSum = 0;

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
  autotunePeriodSumMs = 0;
  autotuneAmplitudeSum = 0;
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
    autotuneCycleCount++;
    double amplitude = (autotuneMaxTemp - autotuneMinTemp) / 2.0;
    autotuneAmplitudeSum += amplitude;
    autotunePeriodSumMs += halfPeriod * 2.0; // approximate full period
    autotuneMinTemp = currentTemperature;
    autotuneMaxTemp = currentTemperature;
  } else if (!autotuneRelayHigh &&
             currentTemperature <= autotuneTargetTemp - AUTOTUNE_HYSTERESIS_C) {
    autotuneRelayHigh = true;
    autotuneLastSwitchTime = now;
  }

  Output = autotuneRelayHigh ? AUTOTUNE_RELAY_HIGH : AUTOTUNE_RELAY_LOW;

  if (autotuneCycleCount >= AUTOTUNE_MIN_CYCLES) {
    double avgPeriodMs = autotunePeriodSumMs / autotuneCycleCount;
    double avgAmplitude = autotuneAmplitudeSum / autotuneCycleCount;
    if (avgAmplitude < 0.1) {
      abortAutotune("oscillation too small to measure - target too close to ambient?");
      return;
    }

    double Pu = avgPeriodMs / 1000.0; // ultimate period, seconds
    double d = (AUTOTUNE_RELAY_HIGH - AUTOTUNE_RELAY_LOW) / 2.0; // relay half-amplitude
    double Ku = (4.0 * d) / (PI * avgAmplitude);                 // ultimate gain

    // Classic Ziegler-Nichols relay-tuning formulas. Kp/Ki/Kd here are in the
    // same per-second units SetTunings() already expects (PID_v1 rescales
    // internally by its own sample time) - matches how the Web UI's manual
    // tuning fields have worked all along.
    double newKp = 0.6 * Ku;
    double newKi = 1.2 * Ku / Pu;
    double newKd = 0.075 * Ku * Pu;

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
    char msg[96];
    snprintf(msg, sizeof(msg), "Done: Ku=%.2f Pu=%.1fs -> Kp=%.2f Ki=%.4f Kd=%.2f",
             Ku, Pu, newKp, newKi, newKd);
    autotuneMessage = msg;
    Serial.println(String("Autotune: ") + msg);

    Output = 0;
    setOpMode(currentMode); // resume normal PID control with the new gains
  }
}

// ============================================================================
// Shot timer + history log
// ----------------------------------------------------------------------------
// Manually triggered from the Web UI (Start/Stop button) - the ESP32 has no
// visibility into the machine's own Brew switch/pump yet. See AGENTS.md
// roadmap item 7 for the sense-only hardware path that would automate this.
// ============================================================================
bool shotInProgress = false;
unsigned long shotStartMillis = 0;
float shotPeakTemp = 0.0;

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
}

void stopShot() {
  if (!shotInProgress) return;
  shotInProgress = false;
  unsigned long durationMs = millis() - shotStartMillis;
  shotLogAppend(time(nullptr), durationMs, shotPeakTemp, 0.0);

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
  steamSetpoint = preferences.getDouble("steam_target", STEAM_SETPOINT_DEFAULT);
  steamKp = preferences.getDouble("steam_kp", STEAM_KP_DEFAULT);
  steamKi = preferences.getDouble("steam_ki", STEAM_KI_DEFAULT);
  steamKd = preferences.getDouble("steam_kd", STEAM_KD_DEFAULT);
  steamMaxSafety = preferences.getDouble("steam_max_safety", STEAM_MAX_SAFETY_DEFAULT);
  ecoTimeoutMin = preferences.getULong("eco_min", ECO_TIMEOUT_MIN_DEFAULT);
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

  // PID Computation - autotune (if running) owns Output directly instead
  if (autotuneState == AutotuneState::RUNNING) {
    runAutotuneStep(now);
  } else if (currentTemperature > 0 && currentTemperature < activeMaxSafety) {
    Input = currentTemperature;
    myPID.Compute();
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
