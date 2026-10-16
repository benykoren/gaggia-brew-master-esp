#pragma once

#include <stdint.h>

// Pure arithmetic for the 3-run pump calibration (design spec 2026-10-02
// §6). The run state machine itself is device-side (pump_calibration.cpp).

#define PUMP_CAL_RUNS 3

struct CalEvaluation {
  float mean;      // mean ml/stroke of the runs
  int outlier;     // index of the run furthest from the mean if > PUMP_CAL_MAX_SPREAD off, else -1
  int implausible; // first run outside PUMP_ML_PER_CLICK_MIN..MAX, else -1
};

// ml per pump stroke for one run; negative if the input can't be a real run.
float calRunMlPerClick(float grams, uint32_t firedCycles);
bool calPlausible(float mlPerClick);
CalEvaluation calEvaluate(const float runs[PUMP_CAL_RUNS]);
