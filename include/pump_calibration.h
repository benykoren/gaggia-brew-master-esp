#pragma once

#include <Arduino.h>

// Pump calibration runner (design spec 2026-10-02 §6): 3 free-flow runs at
// 100% for PUMP_CAL_RUN_MS, the user weighs each cup, the mean ml/stroke
// becomes the pump model's scale. Owns pumpMlPerClick (NVS "pump_mlpc").
// All functions are called with stateMutex held (controlTick / /update).

enum class PumpCalState { IDLE, RUNNING, AWAIT_GRAMS, ABORTED };

extern float pumpMlPerClick; // the model's ml/stroke (default until calibrated)
extern bool pumpCalibrated;  // true once a calibration has been saved

void pumpCalInit();
void pumpCalTick(unsigned long nowMs);
bool pumpCalStartRun(int runIndex, const char **err); // 0..2, or -1 = next empty slot
void pumpCalStop();                                   // Stop button
void pumpCalAbort(const char *reason);                // discards the running run
bool pumpCalRecordGrams(float grams, const char **err);
bool pumpCalSave(const char **err);
void pumpCalClear();                                  // discard unsaved runs
bool pumpCalSetValue(float mlPerClick);               // settings import
PumpCalState pumpCalGetState();
const char *pumpCalStateName();
const char *pumpCalAbortReason();
int pumpCalActiveRun();
unsigned long pumpCalRunElapsedMs(unsigned long nowMs);
float pumpCalRunValue(int i); // < 0 = not measured yet
