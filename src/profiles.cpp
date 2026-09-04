#include "profiles.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include "config.h"

// ============================================================================
// Storage format: JSON array on LittleFS (2026-08-23, replaces hand-rolled
// CSV) - one profile per array entry, same field names the API
// (profilesReadJson()) already returned, so this is now the on-disk format
// AND the wire format with no reserialization step, mirroring GaggiMate's
// own ProfileManager (research pass, 2026-08-23). Fixes the CSV format's
// hard limitation along the way: profile names could not contain a comma or
// newline (silently stripped); JSON has no such restriction.
// ============================================================================

static bool loadDoc(JsonDocument &doc) {
  File f = LittleFS.open(PROFILE_LOG_PATH, "r");
  if (!f) return false;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  return !err;
}

static void saveDoc(JsonDocument &doc) {
  File f = LittleFS.open(PROFILE_LOG_PATH, "w");
  if (!f) {
    Serial.println("profiles: failed to open for write");
    return;
  }
  serializeJson(doc, f);
  f.close();
}

static void addProfile(JsonArray &arr, const char *name, double temp,
                        unsigned long autoStopSec, bool preinfusionEnabled,
                        int pulses, int onMs, int offMs) {
  JsonObject p = arr.add<JsonObject>();
  p["name"] = name;
  p["temp"] = temp;
  p["auto_stop_sec"] = autoStopSec;
  p["preinfusion"] = preinfusionEnabled;
  p["pulses"] = pulses;
  p["on_ms"] = onMs;
  p["off_ms"] = offMs;
  // Pressure control (HARDWARE_ROADMAP.md item 8) - none of the seeded
  // starter profiles use it; disabled by default, same as every other
  // field's "no data yet" convention in this file.
  p["pressure_enabled"] = false;
  p["pressure_ramp_bar"] = 0.0;
  p["pressure_ramp_ms"] = 0;
  p["pressure_decline_enabled"] = false;
  p["pressure_decline_bar"] = 0.0;
  p["pressure_decline_ms"] = 0;
}

void profilesInit() {
  if (!LittleFS.begin(true)) {
    Serial.println("profilesInit: LittleFS mount failed even after format");
    return;
  }
  if (LittleFS.exists(PROFILE_LOG_PATH)) return;

  // Seed starter profiles grounded in real community consensus (2026-08-16
  // research pass) - temp-by-roast from Papel Espresso's roast guide,
  // SCA-referenced shot-time windows, and pulsed pre-infusion timings
  // translated from Profitec/Gicar/Gaggiuino roast-tuning guidance (light
  // roasts benefit from a longer low-pressure soak for CO2 degassing/even
  // saturation; dark roasts need little to none). "Classic" is first/
  // default - straight extraction, no pulsing at all.
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  addProfile(arr, "Classic", 93.0, 27, false, 0, 0, 0);
  addProfile(arr, "Medium Roast", 93.0, 27, true, 3, 1000, 2000);
  addProfile(arr, "Light Roast", 95.0, 28, true, 4, 1000, 3000);
  addProfile(arr, "Dark Roast", 90.0, 26, true, 2, 1000, 1000);
  addProfile(arr, "Ristretto", 93.0, 22, true, 2, 1000, 1000);
  addProfile(arr, "Lungo", 92.0, 40, true, 3, 1000, 2000);
  addProfile(arr, "Decaf", 90.0, 27, true, 3, 1000, 2000);
  saveDoc(doc);
  Serial.println("profiles: seeded starter profiles (Classic/Medium/Light/Dark/Ristretto/Lungo/Decaf)");
}

int profileCount() {
  JsonDocument doc;
  if (!loadDoc(doc)) return 0;
  return doc.as<JsonArray>().size();
}

// The on-disk format IS the wire format now - no rebuilding needed, just
// hand the stored document straight back out.
String profilesReadJson() {
  JsonDocument doc;
  if (!loadDoc(doc)) return "[]";
  String out;
  serializeJson(doc, out);
  return out;
}

bool profileGet(int index, String &name, double &temp, unsigned long &autoStopSec,
                bool &preinfusionEnabled, int &pulses, int &onMs, int &offMs,
                bool &pressureEnabled, double &pressureRampBar, unsigned long &pressureRampMs,
                bool &pressureDeclineEnabled, double &pressureDeclineBar,
                unsigned long &pressureDeclineMs) {
  JsonDocument doc;
  if (!loadDoc(doc)) return false;
  JsonArray arr = doc.as<JsonArray>();
  if (index < 0 || index >= (int)arr.size()) return false;

  JsonObject p = arr[index];
  name = p["name"].as<String>();
  temp = p["temp"].as<double>();
  autoStopSec = p["auto_stop_sec"].as<unsigned long>();
  preinfusionEnabled = p["preinfusion"].as<bool>();
  pulses = p["pulses"].as<int>();
  onMs = p["on_ms"].as<int>();
  offMs = p["off_ms"].as<int>();
  // `| default` so profiles saved before this field existed (missing key)
  // deserialize as "pressure control disabled" instead of an unpredictable
  // JSON-null coercion.
  pressureEnabled = p["pressure_enabled"] | false;
  pressureRampBar = p["pressure_ramp_bar"] | 0.0;
  pressureRampMs = p["pressure_ramp_ms"] | 0UL;
  pressureDeclineEnabled = p["pressure_decline_enabled"] | false;
  pressureDeclineBar = p["pressure_decline_bar"] | 0.0;
  pressureDeclineMs = p["pressure_decline_ms"] | 0UL;
  return true;
}

int profileSave(int index, String name, double temp, unsigned long autoStopSec,
                bool preinfusionEnabled, int pulses, int onMs, int offMs,
                bool pressureEnabled, double pressureRampBar, unsigned long pressureRampMs,
                bool pressureDeclineEnabled, double pressureDeclineBar,
                unsigned long pressureDeclineMs) {
  if (name.length() > PROFILE_NAME_MAX_LEN) name = name.substring(0, PROFILE_NAME_MAX_LEN);
  if (name.length() == 0) name = "Profile";

  JsonDocument doc;
  loadDoc(doc); // ok if the file doesn't exist yet - doc just stays empty
  JsonArray arr = doc.is<JsonArray>() ? doc.as<JsonArray>() : doc.to<JsonArray>();
  int count = arr.size();

  JsonObject target;
  int targetIndex;
  if (index >= 0 && index < count) {
    targetIndex = index;
    target = arr[index];
  } else {
    if (count >= PROFILE_MAX_COUNT) return -1; // full
    targetIndex = count;
    target = arr.add<JsonObject>();
  }
  target["name"] = name;
  target["temp"] = temp;
  target["auto_stop_sec"] = autoStopSec;
  target["preinfusion"] = preinfusionEnabled;
  target["pulses"] = pulses;
  target["on_ms"] = onMs;
  target["off_ms"] = offMs;
  target["pressure_enabled"] = pressureEnabled;
  target["pressure_ramp_bar"] = pressureRampBar;
  target["pressure_ramp_ms"] = pressureRampMs;
  target["pressure_decline_enabled"] = pressureDeclineEnabled;
  target["pressure_decline_bar"] = pressureDeclineBar;
  target["pressure_decline_ms"] = pressureDeclineMs;

  saveDoc(doc);
  return targetIndex;
}

void profileDelete(int index) {
  JsonDocument doc;
  if (!loadDoc(doc)) return;
  JsonArray arr = doc.as<JsonArray>();
  int count = arr.size();
  if (index < 0 || index >= count || count <= 1) return; // always keep at least one
  arr.remove(index);
  saveDoc(doc);
}
