#include "pump_cal_math.h"

#include <math.h>

#include "config.h"

float calRunMlPerClick(float grams, uint32_t firedCycles) {
  if (firedCycles == 0 || !(grams > 0.0f)) return -1.0f;
  return grams / (float)firedCycles;
}

bool calPlausible(float mlPerClick) {
  return mlPerClick >= PUMP_ML_PER_CLICK_MIN && mlPerClick <= PUMP_ML_PER_CLICK_MAX;
}

CalEvaluation calEvaluate(const float runs[PUMP_CAL_RUNS]) {
  CalEvaluation e = {0.0f, -1, -1};
  float sum = 0.0f;
  for (int i = 0; i < PUMP_CAL_RUNS; i++) {
    if (e.implausible < 0 && !calPlausible(runs[i])) e.implausible = i;
    sum += runs[i];
  }
  e.mean = sum / PUMP_CAL_RUNS;
  if (!(e.mean > 0.0f)) return e;
  float worst = PUMP_CAL_MAX_SPREAD;
  for (int i = 0; i < PUMP_CAL_RUNS; i++) {
    float dev = fabsf(runs[i] - e.mean) / e.mean;
    if (dev > worst) {
      worst = dev;
      e.outlier = i;
    }
  }
  return e;
}
