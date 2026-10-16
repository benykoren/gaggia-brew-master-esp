#pragma once

// Open-loop pump/puck model (design spec 2026-10-02 §4.1). Pure C++ - no
// Arduino headers - so it builds and unit-tests on the PC (env:native).
//
// Pump: flow = duty x strokes/s x ml-per-stroke(P). Puck: the reference
// puck's flow, linear in P, scaled by the profile's trim. The duty laws pick
// the duty that keeps the two balanced at a target (plus extra flow to
// compress the system when the target is above the estimate), and
// pumpModelStep() integrates the resulting estimated pressure P_est.

struct PumpModelParams {
  float mlPerClick; // calibrated free-flow ml per pump stroke
  float cps;        // strokes per second at 100% duty (= measured mains Hz)
  float trimPct;    // -20..+20, scales the reference puck's flow
};

struct PumpModelState {
  float pressureBar; // P_est
  float mlPumped;    // ml_est
};

// ml-per-stroke at `bar` relative to free flow: 1.0 at 0 bar, falling along
// Gaggiuino's EP5 curve, never below PUMP_MODEL_SHAPE_FLOOR.
float pumpShape(float bar);
// ml/s at 100% duty against `bar`.
float pumpFlowFull(const PumpModelParams &m, float bar);
// ml/s the reference puck passes at `bar`.
float puckFlow(const PumpModelParams &m, float bar);
// Duty (0..1) to move P from pressureNow toward targetBar with time
// constant PUMP_MODEL_TAU_S, and hold it there.
float dutyForPressure(const PumpModelParams &m, float pressureNow, float targetBar);
// Duty (0..1) to pump flowMlS against pressureNow.
float dutyForFlow(const PumpModelParams &m, float pressureNow, float flowMlS);

void pumpModelReset(PumpModelState &s);
// Advances P_est/ml_est by dtS seconds at `duty` (0..1).
void pumpModelStep(const PumpModelParams &m, PumpModelState &s, float duty, float dtS);
