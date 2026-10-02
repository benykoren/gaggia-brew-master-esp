#include "shot_planner.h"

#include <math.h>

float curveEval(Curve c, float x) {
  if (!(x > 0.0f)) x = 0.0f;
  if (x > 1.0f) x = 1.0f;
  switch (c) {
    case Curve::LINEAR: return x;
    case Curve::EASE_IN: return x * x * x;
    case Curve::EASE_OUT: { float u = 1.0f - x; return 1.0f - u * u * u; }
    case Curve::EASE_IN_OUT: {
      if (x < 0.5f) return 4.0f * x * x * x;
      float u = -2.0f * x + 2.0f;
      return 1.0f - u * u * u / 2.0f;
    }
    default: return 1.0f; // INSTANT
  }
}

void ShotPlanner::start(const Profile &profile, const PumpModelParams &model) {
  profile_ = profile;
  model_ = model;
  pumpModelReset(state_);
  phaseIdx_ = 0;
  phaseElapsed_ = 0.0f;
  elapsed_ = 0.0f;
  hasPrev_ = false;
  lastDuty_ = 0.0f;
  lastFlow_ = 0.0f;
  running_ = profile_.phaseCount > 0;
  if (running_) enterPhase(0.0f);
}

void ShotPlanner::stop() { running_ = false; }

// Where the new phase's ramp starts (design spec §3.2): an explicit start;
// else the previous phase's reached value if it was the same type; else
// the live state in this phase's unit (pressure now, flow now, duty now).
void ShotPlanner::enterPhase(float pressureNow) {
  const Phase &ph = profile_.phases[phaseIdx_];
  float scale = profile_.pressureScale;
  phaseEnd_ = ph.type == PhaseType::PRESSURE ? ph.target * scale : ph.target;
  if (ph.hasStart) {
    phaseStart_ = ph.type == PhaseType::PRESSURE ? ph.start * scale : ph.start;
  } else if (hasPrev_ && prevType_ == ph.type && ph.type != PhaseType::SOAK) {
    phaseStart_ = prevEnd_;
  } else if (ph.type == PhaseType::PRESSURE) {
    phaseStart_ = pressureNow;
  } else if (ph.type == PhaseType::FLOW) {
    phaseStart_ = lastFlow_;
  } else if (ph.type == PhaseType::POWER) {
    phaseStart_ = lastDuty_ * 100.0f;
  } else {
    phaseStart_ = 0.0f;
  }
}

float ShotPlanner::targetAt(float t) const {
  const Phase &ph = profile_.phases[phaseIdx_];
  if (ph.curve == Curve::INSTANT || !(ph.rampS > 0.0f)) return phaseEnd_;
  return phaseStart_ + (phaseEnd_ - phaseStart_) * curveEval(ph.curve, t / ph.rampS);
}

PlannerOutput ShotPlanner::tick(float dtS, float measuredBar, bool sensorUsable) {
  PlannerOutput o = {};
  o.phaseIndex = -1;
  o.phaseName = "";
  if (!running_) return o;
  if (!(dtS > 0.0f)) dtS = 0.0f;
  elapsed_ += dtS;
  phaseElapsed_ += dtS;
  float pNow = sensorUsable ? measuredBar : state_.pressureBar;

  // Advance through every phase whose time (or stop condition) is up -
  // carrying leftover time so a long tick can't stretch the shot.
  while (phaseIdx_ < profile_.phaseCount) {
    const Phase &ph = profile_.phases[phaseIdx_];
    bool timeUp = phaseElapsed_ >= ph.durationS;
    bool stopHit = sensorUsable &&
                   ((ph.stopPressGte >= 0.0f && measuredBar >= scaledStop(ph.stopPressGte)) ||
                    (ph.stopPressLte >= 0.0f && measuredBar <= scaledStop(ph.stopPressLte)));
    if (!timeUp && !stopHit) break;
    float carry = timeUp ? phaseElapsed_ - ph.durationS : 0.0f;
    prevEnd_ = targetAt(timeUp ? ph.durationS : phaseElapsed_);
    prevType_ = ph.type;
    hasPrev_ = true;
    phaseIdx_++;
    phaseElapsed_ = carry;
    if (phaseIdx_ < profile_.phaseCount) enterPhase(pNow);
  }

  if (phaseIdx_ >= profile_.phaseCount || elapsed_ >= (float)profile_.maxSec) {
    running_ = false;
    o.done = true;
    o.phaseCount = profile_.phaseCount;
    o.pressureEst = state_.pressureBar;
    o.mlEst = state_.mlPumped;
    o.elapsedS = elapsed_;
    return o;
  }

  const Phase &ph = profile_.phases[phaseIdx_];
  float target = targetAt(phaseElapsed_);
  float duty = 0.0f;
  switch (ph.type) {
    case PhaseType::PRESSURE:
      duty = dutyForPressure(model_, pNow, target);
      if (ph.limit > 0.0f) duty = fminf(duty, dutyForFlow(model_, pNow, ph.limit));
      break;
    case PhaseType::FLOW: {
      float limitBar = ph.limit > 0.0f ? ph.limit * profile_.pressureScale : 0.0f;
      float pForFlow = (limitBar > 0.0f && pNow > limitBar) ? limitBar : pNow;
      duty = dutyForFlow(model_, pForFlow, target);
      if (limitBar > 0.0f) duty = fminf(duty, dutyForPressure(model_, pNow, limitBar));
      break;
    }
    case PhaseType::POWER: duty = target / 100.0f; break;
    default: duty = 0.0f; break; // SOAK
  }
  if (!(duty >= 0.0f)) duty = 0.0f;
  if (duty > 1.0f) duty = 1.0f;

  pumpModelStep(model_, state_, duty, dtS);
  lastDuty_ = duty;
  lastFlow_ = duty * pumpFlowFull(model_, pNow);

  o.running = true;
  o.dutyPct = duty * 100.0f;
  o.phaseIndex = phaseIdx_;
  o.phaseCount = profile_.phaseCount;
  o.phaseName = ph.name;
  o.phaseType = ph.type;
  o.phaseTarget = target;
  o.pressureEst = state_.pressureBar;
  o.mlEst = state_.mlPumped;
  o.elapsedS = elapsed_;
  return o;
}
