#pragma once

#include "profile_types.h"
#include "pump_model.h"

// Phase sequencer (design spec 2026-10-02 §4.2). Pure C++: time comes in as
// dtS per tick, so tests drive it without millis(). The caller (main.cpp's
// controlTick, every 50 ms under stateMutex) sends dutyPct to the dimmer;
// the safety cutoffs that run after it still win.

// Ease curve value for x in 0..1 (clamped). INSTANT returns 1.
float curveEval(Curve c, float x);

struct PlannerOutput {
  bool running;
  bool done;            // true on the tick the shot finished (phases or max_sec)
  float dutyPct;        // 0..100
  int phaseIndex;       // -1 when idle
  int phaseCount;
  const char *phaseName; // points into the planner's own profile copy; "" when idle
  PhaseType phaseType;
  float phaseTarget;    // current interpolated target, in the phase's own unit
  float pressureEst;    // P_est (bar)
  float mlEst;          // estimated water pumped this shot (ml)
  float elapsedS;
};

class ShotPlanner {
 public:
  void start(const Profile &profile, const PumpModelParams &model);
  // measuredBar/sensorUsable: the real sensor reading, only trusted (for
  // duty laws and stop conditions) when sensorUsable is true.
  PlannerOutput tick(float dtS, float measuredBar, bool sensorUsable);
  void stop();
  bool running() const { return running_; }

 private:
  void enterPhase(float pressureNow);
  float targetAt(float t) const;
  float scaledStop(float v) const { return v * profile_.pressureScale; }

  Profile profile_ = {};
  PumpModelParams model_ = {};
  PumpModelState state_ = {};
  bool running_ = false;
  int phaseIdx_ = 0;
  float phaseElapsed_ = 0.0f;
  float elapsed_ = 0.0f;
  float phaseStart_ = 0.0f;
  float phaseEnd_ = 0.0f;
  bool hasPrev_ = false;
  PhaseType prevType_ = PhaseType::SOAK;
  float prevEnd_ = 0.0f;
  float lastDuty_ = 0.0f;
  float lastFlow_ = 0.0f;
};
