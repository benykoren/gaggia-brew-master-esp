#pragma once

#include <stdint.h>

// Water tank level sensor driver (HARDWARE_ROADMAP.md item 6).
//
// A single digital input (PIN_WATER_LEVEL) from a non-contact capacitive
// switch: "is there water at the sensor's height". Sampled from the control
// task and debounced (WATER_DEBOUNCE_SAMPLES consistent reads) so sloshing
// or reseating the tank never flickers the state. No web/MQTT knowledge
// here - main.cpp owns the settings and the shot interlock.

enum class WaterState : uint8_t { DISABLED, OK, LOW_WATER };

void waterLevelInit();

// Settings (persisted by the caller). Changing either resets the debounce
// so the new interpretation takes effect from a clean slate.
void waterLevelConfigure(bool enabled, bool activeLow);

// Call every control tick; samples at WATER_SAMPLE_INTERVAL_MS.
void waterLevelTick(unsigned long now);

// Debounced state. DISABLED when the sensor is switched off in Settings.
WaterState waterLevelState();

// Raw pin level from the most recent sample (true = HIGH), regardless of
// enabled/polarity - shown live in Settings for calibrating after install.
bool waterLevelRawHigh();

// "low" / "ok" / "disabled" - the string used in /status and MQTT.
const char *waterStateName(WaterState s);
