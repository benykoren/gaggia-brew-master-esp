#include "water_level.h"

#include <Arduino.h>

#include "config.h"

static bool enabled = WATER_SENSOR_ENABLED_DEFAULT;
static bool activeLow = WATER_ACTIVE_LOW_DEFAULT;

static bool rawHigh = false;
static unsigned long lastSampleMs = 0;
// Debounced "water present". Starts true so a fresh boot (or re-enable)
// never shows a false "Low water" before a full debounce window has passed.
static bool waterPresent = true;
static int disagreeCount = 0;

void waterLevelInit() {
  // Pull-up: harmless with the sensor's push-pull output, keeps the pin at
  // a defined level while nothing is wired yet (sensor disabled by default).
  pinMode(PIN_WATER_LEVEL, INPUT_PULLUP);
  rawHigh = digitalRead(PIN_WATER_LEVEL) == HIGH;
}

void waterLevelConfigure(bool en, bool actLow) {
  if (en != enabled || actLow != activeLow) {
    waterPresent = true;
    disagreeCount = 0;
  }
  enabled = en;
  activeLow = actLow;
}

void waterLevelTick(unsigned long now) {
  if ((long)(now - lastSampleMs) < WATER_SAMPLE_INTERVAL_MS) return;
  lastSampleMs = now;

  rawHigh = digitalRead(PIN_WATER_LEVEL) == HIGH;
  if (!enabled) return;

  bool readsWater = activeLow ? !rawHigh : rawHigh;
  if (readsWater == waterPresent) {
    disagreeCount = 0;
    return;
  }
  if (++disagreeCount >= WATER_DEBOUNCE_SAMPLES) {
    waterPresent = readsWater;
    disagreeCount = 0;
    Serial.println(waterPresent ? "Water level: OK" : "Water level: LOW");
  }
}

WaterState waterLevelState() {
  if (!enabled) return WaterState::Disabled;
  return waterPresent ? WaterState::Ok : WaterState::Low;
}

bool waterLevelRawHigh() { return rawHigh; }

const char *waterStateName(WaterState s) {
  switch (s) {
    case WaterState::Ok: return "ok";
    case WaterState::Low: return "low";
    default: return "disabled";
  }
}
