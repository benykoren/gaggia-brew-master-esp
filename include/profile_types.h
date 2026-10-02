#pragma once

#include <stdint.h>

#include "config.h"

// Phase-list shot profiles (design spec 2026-10-02 §3.2). Plain structs -
// shared by the pure codec/planner and the device-side store.

enum class PhaseType : uint8_t { PRESSURE, FLOW, POWER, SOAK };
enum class Curve : uint8_t { INSTANT, LINEAR, EASE_IN, EASE_OUT, EASE_IN_OUT };

struct Phase {
  char name[PHASE_NAME_MAX_LEN + 1];
  PhaseType type;
  float target;       // bar (PRESSURE), ml/s (FLOW), % (POWER); ignored for SOAK
  float limit;        // PRESSURE: max ml/s; FLOW: max bar; 0 = none
  bool hasStart;      // explicit ramp start value (built-ins only for now)
  float start;
  Curve curve;
  float rampS;        // seconds to move from the start value to target
  float durationS;    // hard cap; the only exit without a usable sensor
  float stopPressGte; // < 0 = none; only evaluated with a usable sensor
  float stopPressLte; // < 0 = none
};

struct Profile {
  char name[PROFILE_NAME_MAX_LEN + 1];
  bool builtin;
  bool utility;        // Flush/Backflush: no temp setpoint, not logged
  float temp;
  uint16_t maxSec;
  float pressureScale; // multiplies pressure targets, flow-phase bar limits, press_* stops
  int8_t trimPct;      // scales the reference puck (pump_model.h)
  uint8_t phaseCount;
  Phase phases[PROFILE_UTILITY_PHASES_MAX];
};

// The user-editable subset (allowed on built-ins too, stored as overrides).
struct ProfileTweak {
  float temp;
  uint16_t maxSec;
  float pressureScale;
  int8_t trimPct;
};
