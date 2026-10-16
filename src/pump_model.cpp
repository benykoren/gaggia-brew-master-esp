#include "pump_model.h"

#include "config.h"

// Gaggiuino EP5 flow-per-click polynomial (release/stm32-blackpill,
// src/peripherals/pump.cpp, pressureInefficiencyCoefficient), evaluated at
// their default pumpFlowAtZero. Only its SHAPE is used - normalized so
// pumpShape(0) == 1 - the absolute scale comes from our own calibration.
static const float K0 = 0.045f, K1 = 0.015f, K2 = 0.0033f, K3 = 0.000685f, K4 = 0.000045f,
                   K5 = 0.009f, K6 = -0.0018f;
static const float GAGGIUINO_F0 = 0.2225f;

static float gaggiuinoFpc(float p) {
  // (K5/P + K6) * -P^2 expanded so P = 0 is well defined.
  return -K5 * p - K6 * p * p + (GAGGIUINO_F0 - K0) - (K1 + (K2 - (K3 - K4 * p) * p) * p) * p;
}

static float clamp01(float v) {
  if (!(v >= 0.0f)) return 0.0f; // also catches NaN
  return v > 1.0f ? 1.0f : v;
}

float pumpShape(float bar) {
  if (!(bar >= 0.0f)) bar = 0.0f;
  float s = gaggiuinoFpc(bar) / gaggiuinoFpc(0.0f);
  return s < PUMP_MODEL_SHAPE_FLOOR ? PUMP_MODEL_SHAPE_FLOOR : s;
}

float pumpFlowFull(const PumpModelParams &m, float bar) {
  return m.cps * m.mlPerClick * pumpShape(bar);
}

float puckFlow(const PumpModelParams &m, float bar) {
  if (!(bar > 0.0f)) return 0.0f;
  return PUMP_MODEL_REF_PUCK_FLOW_ML_S * (1.0f + m.trimPct / 100.0f) * bar / PUMP_MODEL_REF_PUCK_BAR;
}

float dutyForPressure(const PumpModelParams &m, float pressureNow, float targetBar) {
  float q = pumpFlowFull(m, pressureNow);
  if (!(q > 0.0f)) return 0.0f;
  float need = puckFlow(m, pressureNow) +
               PUMP_MODEL_COMPLIANCE_ML_PER_BAR * (targetBar - pressureNow) / PUMP_MODEL_TAU_S;
  return clamp01(need / q);
}

float dutyForFlow(const PumpModelParams &m, float pressureNow, float flowMlS) {
  float q = pumpFlowFull(m, pressureNow);
  if (!(q > 0.0f) || !(flowMlS > 0.0f)) return 0.0f;
  return clamp01(flowMlS / q);
}

void pumpModelReset(PumpModelState &s) {
  s.pressureBar = 0.0f;
  s.mlPumped = 0.0f;
}

void pumpModelStep(const PumpModelParams &m, PumpModelState &s, float duty, float dtS) {
  if (!(dtS > 0.0f)) return;
  float flow = clamp01(duty) * pumpFlowFull(m, s.pressureBar);
  s.mlPumped += flow * dtS;
  if (s.mlPumped < PUMP_MODEL_HEADSPACE_ML) {
    s.pressureBar = 0.0f; // still filling the headspace / wetting the puck
    return;
  }
  s.pressureBar += (flow - puckFlow(m, s.pressureBar)) / PUMP_MODEL_COMPLIANCE_ML_PER_BAR * dtS;
  if (s.pressureBar < 0.0f) s.pressureBar = 0.0f;
  if (s.pressureBar > PUMP_MODEL_MAX_BAR) s.pressureBar = PUMP_MODEL_MAX_BAR;
}
