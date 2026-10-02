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
