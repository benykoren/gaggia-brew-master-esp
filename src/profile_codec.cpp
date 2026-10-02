#include "profile_codec.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// ============================================================================
// Built-in profiles (design spec 2026-10-02 §3.3) - translated from
// Gaggiuino/GaggiMate defaults so every phase also exits on its own
// duration_s (no usable sensor yet). Pressure/flow values are model targets
// (pump_model.cpp), not measurements. "stop" conditions only apply once a
// 0-16 bar sensor is fitted (PRESSURE_CLOSED_LOOP_AVAILABLE).
// ============================================================================
static const char BUILTIN_PROFILES_JSON[] = R"json([
{"name":"Classic 9 bar","temp":93,"max_sec":35,"phases":[
 {"name":"Fill","type":"flow","target":4,"limit":4,"duration_s":6,"stop":{"press_gte":4}},
 {"name":"Ramp","type":"pressure","target":9,"curve":"linear","ramp_s":3,"duration_s":3},
 {"name":"Hold","type":"pressure","target":9,"duration_s":26}]},
{"name":"Londinium","temp":92,"max_sec":45,"phases":[
 {"name":"Fill","type":"flow","target":6,"limit":4,"duration_s":8,"stop":{"press_gte":4}},
 {"name":"Soak","type":"soak","duration_s":10},
 {"name":"Rise","type":"pressure","target":9,"curve":"ease-out","ramp_s":2,"duration_s":2},
 {"name":"Hold","type":"pressure","target":9,"limit":3,"duration_s":4},
 {"name":"Decline","type":"pressure","target":3,"limit":3,"curve":"linear","ramp_s":20,"duration_s":20}]},
{"name":"Lever","temp":88,"max_sec":50,"phases":[
 {"name":"Pre-infuse","type":"pressure","target":1.5,"duration_s":8},
 {"name":"Hold","type":"pressure","target":1.5,"duration_s":7},
 {"name":"Rise","type":"pressure","target":9,"curve":"ease-in-out","ramp_s":8,"duration_s":8},
 {"name":"Decline","type":"pressure","target":3,"curve":"linear","ramp_s":25,"duration_s":25}]},
{"name":"IUIUIU Classic","temp":93,"max_sec":45,"phases":[
 {"name":"Pre-infuse","type":"flow","target":3,"limit":4,"duration_s":10,"stop":{"press_gte":4}},
 {"name":"Soak","type":"soak","duration_s":8},
 {"name":"Rise","type":"pressure","target":7.5,"curve":"ease-out","ramp_s":5,"duration_s":5},
 {"name":"Ease","type":"pressure","target":6,"limit":3,"curve":"ease-in-out","ramp_s":4,"duration_s":4},
 {"name":"Flow","type":"flow","target":1,"start":2.5,"limit":6,"curve":"ease-in-out","ramp_s":15,"duration_s":15}]},
{"name":"Blooming","temp":93,"max_sec":55,"phases":[
 {"name":"Fill","type":"flow","target":4,"limit":4,"duration_s":8,"stop":{"press_gte":4}},
 {"name":"Bloom","type":"soak","duration_s":25},
 {"name":"Ramp","type":"flow","target":2,"limit":6,"curve":"ease-out","ramp_s":5,"duration_s":5},
 {"name":"Flow","type":"flow","target":2,"limit":6,"duration_s":15}]},
{"name":"Flush","utility":true,"temp":0,"max_sec":10,"phases":[
 {"name":"Flush","type":"power","target":100,"duration_s":5}]},
{"name":"Backflush","utility":true,"temp":0,"max_sec":100,"phases":[
 {"name":"On 1","type":"power","target":100,"duration_s":10},{"name":"Off 1","type":"soak","duration_s":10},
 {"name":"On 2","type":"power","target":100,"duration_s":10},{"name":"Off 2","type":"soak","duration_s":10},
 {"name":"On 3","type":"power","target":100,"duration_s":10},{"name":"Off 3","type":"soak","duration_s":10},
 {"name":"On 4","type":"power","target":100,"duration_s":10},{"name":"Off 4","type":"soak","duration_s":10},
 {"name":"On 5","type":"power","target":100,"duration_s":10},{"name":"Off 5","type":"soak","duration_s":10}]}
])json";

static void copyStr(char *dst, size_t cap, const char *src) {
  if (!src) src = "";
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

static bool inRange(float v, float lo, float hi) { return v >= lo && v <= hi; } // false for NaN

static float clampf(float v, float lo, float hi) {
  if (!(v >= lo)) return lo; // NaN -> lo
  return v > hi ? hi : v;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

const char *phaseTypeName(PhaseType t) {
  switch (t) {
    case PhaseType::PRESSURE: return "pressure";
    case PhaseType::FLOW: return "flow";
    case PhaseType::POWER: return "power";
    default: return "soak";
  }
}

static const char *curveName(Curve c) {
  switch (c) {
    case Curve::LINEAR: return "linear";
    case Curve::EASE_IN: return "ease-in";
    case Curve::EASE_OUT: return "ease-out";
    case Curve::EASE_IN_OUT: return "ease-in-out";
    default: return "instant";
  }
}

static bool parseType(const char *s, PhaseType &t) {
  if (!s) return false;
  if (!strcmp(s, "pressure")) t = PhaseType::PRESSURE;
  else if (!strcmp(s, "flow")) t = PhaseType::FLOW;
  else if (!strcmp(s, "power")) t = PhaseType::POWER;
  else if (!strcmp(s, "soak")) t = PhaseType::SOAK;
  else return false;
  return true;
}

static bool parseCurve(const char *s, Curve &c) {
  if (!s || !strcmp(s, "instant")) c = Curve::INSTANT;
  else if (!strcmp(s, "linear")) c = Curve::LINEAR;
  else if (!strcmp(s, "ease-in")) c = Curve::EASE_IN;
  else if (!strcmp(s, "ease-out")) c = Curve::EASE_OUT;
  else if (!strcmp(s, "ease-in-out")) c = Curve::EASE_IN_OUT;
  else return false;
  return true;
}

static bool targetInRange(PhaseType t, float v) {
  switch (t) {
    case PhaseType::PRESSURE: return inRange(v, 0.0f, PHASE_PRESSURE_MAX_BAR);
    case PhaseType::FLOW: return inRange(v, 0.0f, PHASE_FLOW_MAX_ML_S);
    case PhaseType::POWER: return inRange(v, 0.0f, 100.0f);
    default: return true; // SOAK ignores its target
  }
}

bool profileValidate(const Profile &p, const char **err) {
  if (p.name[0] == '\0') { *err = "Name is empty"; return false; }
  int maxPhases = p.utility ? PROFILE_UTILITY_PHASES_MAX : PROFILE_PHASES_MAX;
  if (p.phaseCount < 1) { *err = "Profile has no phases"; return false; }
  if (p.phaseCount > maxPhases) { *err = "Too many phases"; return false; }
  if (!p.utility && !inRange(p.temp, PROFILE_TEMP_MIN, PROFILE_TEMP_MAX)) { *err = "Temperature out of range (80-100)"; return false; }
  int maxSecMax = p.utility ? PROFILE_UTILITY_MAX_SEC_MAX : PROFILE_MAX_SEC_MAX;
  if (p.maxSec < PROFILE_MAX_SEC_MIN || p.maxSec > maxSecMax) { *err = "Max shot time out of range"; return false; }
  if (!inRange(p.pressureScale, PROFILE_PRESSURE_SCALE_MIN, PROFILE_PRESSURE_SCALE_MAX)) { *err = "Pressure scale out of range (0.7-1.1)"; return false; }
  if (p.trimPct < PROFILE_TRIM_MIN || p.trimPct > PROFILE_TRIM_MAX) { *err = "Trim out of range (-20..20)"; return false; }
  for (int i = 0; i < p.phaseCount; i++) {
    const Phase &f = p.phases[i];
    if (!inRange(f.durationS, PHASE_DURATION_MIN_S, PHASE_DURATION_MAX_S)) { *err = "Phase duration out of range"; return false; }
    if (!inRange(f.rampS, 0.0f, f.durationS)) { *err = "Phase ramp longer than the phase"; return false; }
    if (!targetInRange(f.type, f.target)) { *err = "Phase target out of range"; return false; }
    if (f.hasStart && !targetInRange(f.type, f.start)) { *err = "Phase start out of range"; return false; }
    float limitMax = f.type == PhaseType::FLOW ? PHASE_PRESSURE_MAX_BAR : PHASE_FLOW_MAX_ML_S;
    if (!inRange(f.limit, 0.0f, limitMax)) { *err = "Phase limit out of range"; return false; }
    if (f.stopPressGte >= 0.0f && f.stopPressGte > PHASE_PRESSURE_MAX_BAR) { *err = "Stop pressure out of range"; return false; }
    if (f.stopPressLte >= 0.0f && f.stopPressLte > PHASE_PRESSURE_MAX_BAR) { *err = "Stop pressure out of range"; return false; }
  }
  return true;
}

bool profileFromJson(JsonObjectConst o, Profile &p, const char **err) {
  memset(&p, 0, sizeof p);
  const char *name = o["name"].as<const char *>();
  if (!name || !*name) { *err = "Missing name"; return false; }
  if (strlen(name) > PROFILE_NAME_MAX_LEN) { *err = "Name longer than 20 characters"; return false; }
  copyStr(p.name, sizeof p.name, name);
  p.utility = o["utility"] | false;
  p.temp = o["temp"] | 93.0f;
  int maxSec = o["max_sec"] | 30;
  int trim = o["trim_pct"] | 0;
  if (maxSec < 0 || maxSec > 600) { *err = "Max shot time out of range"; return false; }
  if (trim < PROFILE_TRIM_MIN || trim > PROFILE_TRIM_MAX) { *err = "Trim out of range (-20..20)"; return false; }
  p.maxSec = (uint16_t)maxSec;
  p.trimPct = (int8_t)trim;
  p.pressureScale = o["pressure_scale"] | 1.0f;

  JsonArrayConst phases = o["phases"].as<JsonArrayConst>();
  if (phases.isNull()) { *err = "Missing phases"; return false; }
  if (phases.size() > PROFILE_UTILITY_PHASES_MAX) { *err = "Too many phases"; return false; }
  for (JsonVariantConst v : phases) {
    JsonObjectConst j = v.as<JsonObjectConst>();
    Phase &f = p.phases[p.phaseCount];
    copyStr(f.name, sizeof f.name, j["name"].as<const char *>());
    if (!parseType(j["type"].as<const char *>(), f.type)) { *err = "Unknown phase type"; return false; }
    if (!parseCurve(j["curve"].as<const char *>(), f.curve)) { *err = "Unknown curve"; return false; }
    f.target = j["target"] | 0.0f;
    f.limit = j["limit"] | 0.0f;
    f.hasStart = !j["start"].isNull();
    f.start = j["start"] | 0.0f;
    f.rampS = j["ramp_s"] | 0.0f;
    f.durationS = j["duration_s"] | 0.0f;
    f.stopPressGte = j["stop"]["press_gte"] | -1.0f;
    f.stopPressLte = j["stop"]["press_lte"] | -1.0f;
    p.phaseCount++;
  }
  return profileValidate(p, err);
}

void profileToJson(const Profile &p, JsonObject o) {
  o["name"] = p.name;
  o["builtin"] = p.builtin;
  o["utility"] = p.utility;
  o["temp"] = p.temp;
  o["max_sec"] = p.maxSec;
  o["pressure_scale"] = p.pressureScale;
  o["trim_pct"] = p.trimPct;
  JsonArray phases = o["phases"].to<JsonArray>();
  for (int i = 0; i < p.phaseCount; i++) {
    const Phase &f = p.phases[i];
    JsonObject j = phases.add<JsonObject>();
    j["name"] = f.name;
    j["type"] = phaseTypeName(f.type);
    j["target"] = f.target;
    if (f.limit > 0.0f) j["limit"] = f.limit;
    if (f.hasStart) j["start"] = f.start;
    if (f.curve != Curve::INSTANT) j["curve"] = curveName(f.curve);
    if (f.rampS > 0.0f) j["ramp_s"] = f.rampS;
    j["duration_s"] = f.durationS;
    if (f.stopPressGte >= 0.0f || f.stopPressLte >= 0.0f) {
      JsonObject s = j["stop"].to<JsonObject>();
      if (f.stopPressGte >= 0.0f) s["press_gte"] = f.stopPressGte;
      if (f.stopPressLte >= 0.0f) s["press_lte"] = f.stopPressLte;
    }
  }
}

int builtinProfilesLoad(Profile *out, int maxOut) {
  JsonDocument doc;
  if (deserializeJson(doc, BUILTIN_PROFILES_JSON)) return 0;
  int n = 0;
  for (JsonVariantConst v : doc.as<JsonArrayConst>()) {
    if (n >= maxOut) break;
    const char *err = nullptr;
    if (!profileFromJson(v.as<JsonObjectConst>(), out[n], &err)) continue;
    out[n].builtin = true;
    n++;
  }
  return n;
}

ProfileTweak profileGetTweak(const Profile &p) { return {p.temp, p.maxSec, p.pressureScale, p.trimPct}; }

void profileApplyTweak(Profile &p, const ProfileTweak &t) {
  if (!p.utility) p.temp = clampf(t.temp, PROFILE_TEMP_MIN, PROFILE_TEMP_MAX);
  int maxSecMax = p.utility ? PROFILE_UTILITY_MAX_SEC_MAX : PROFILE_MAX_SEC_MAX;
  p.maxSec = (uint16_t)clampi(t.maxSec, PROFILE_MAX_SEC_MIN, maxSecMax);
  p.pressureScale = clampf(t.pressureScale, PROFILE_PRESSURE_SCALE_MIN, PROFILE_PRESSURE_SCALE_MAX);
  p.trimPct = (int8_t)clampi(t.trimPct, PROFILE_TRIM_MIN, PROFILE_TRIM_MAX);
}

bool profileNameEquals(const char *a, const char *b) {
  if (!a || !b) return false;
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    a++;
    b++;
  }
  return *a == *b;
}

void profileMakeOldName(const char *in, char *out, size_t cap) {
  static const char SUFFIX[] = " (old)";
  size_t keep = PROFILE_NAME_MAX_LEN - (sizeof SUFFIX - 1);
  char base[PROFILE_NAME_MAX_LEN + 1];
  copyStr(base, keep + 1, in);
  snprintf(out, cap, "%s%s", base, SUFFIX);
}

static bool nameTaken(const char *name, const Profile *a, int an, const Profile *b, int bn) {
  for (int i = 0; i < an; i++) if (profileNameEquals(name, a[i].name)) return true;
  for (int i = 0; i < bn; i++) if (profileNameEquals(name, b[i].name)) return true;
  return false;
}

static void addPhase(Profile &p, const char *name, PhaseType t, float target, float durS) {
  Phase &f = p.phases[p.phaseCount++];
  memset(&f, 0, sizeof f);
  copyStr(f.name, sizeof f.name, name);
  f.type = t;
  f.target = target;
  f.curve = Curve::INSTANT;
  f.durationS = clampf(durS, PHASE_DURATION_MIN_S, PHASE_DURATION_MAX_S);
  f.stopPressGte = -1.0f;
  f.stopPressLte = -1.0f;
}

int migrateLegacyProfiles(const char *legacyJson, const LegacyPowers &powers, const Profile *taken,
                          int takenCount, Profile *out, int maxOut, int *warnings) {
  *warnings = 0;
  JsonDocument doc;
  if (deserializeJson(doc, legacyJson) || !doc.is<JsonArray>()) return 0;
  int n = 0;
  for (JsonVariantConst v : doc.as<JsonArrayConst>()) {
    JsonObjectConst o = v.as<JsonObjectConst>();
    if (n >= maxOut) { (*warnings)++; break; }
    const char *name = o["name"].as<const char *>();
    if (!name || !*name) { (*warnings)++; continue; }

    Profile p;
    memset(&p, 0, sizeof p);
    copyStr(p.name, sizeof p.name, name);
    if (nameTaken(p.name, taken, takenCount, out, n)) {
      char renamed[PROFILE_NAME_MAX_LEN + 1];
      profileMakeOldName(p.name, renamed, sizeof renamed);
      copyStr(p.name, sizeof p.name, renamed);
    }
    p.temp = clampf(o["temp"] | 93.0f, PROFILE_TEMP_MIN, PROFILE_TEMP_MAX);
    int autoStop = o["auto_stop_sec"] | 27;
    p.maxSec = (uint16_t)(autoStop <= 0 ? PROFILE_MAX_SEC_MAX : clampi(autoStop, PROFILE_MAX_SEC_MIN, PROFILE_MAX_SEC_MAX));
    p.pressureScale = 1.0f;
    p.trimPct = 0;

    bool press = o["pressure_enabled"] | false;
    bool decline = press && (o["pressure_decline_enabled"] | false);
    int tail = press ? (decline ? 3 : 2) : 1;
    int pulses = (o["preinfusion"] | false) ? (int)(o["pulses"] | 0) : 0;
    int maxPulses = (PROFILE_PHASES_MAX - tail + 1) / 2;
    if (pulses > maxPulses) { pulses = maxPulses; (*warnings)++; }
    if (pulses < 0) pulses = 0;
    float onS = (float)(o["on_ms"] | 1000) / 1000.0f;
    float offS = (float)(o["off_ms"] | 1000) / 1000.0f;
    float piPct = (float)clampi(powers.preinfusionPct, 10, 100);

    float used = 0.0f;
    char label[PHASE_NAME_MAX_LEN + 1];
    for (int i = 0; i < pulses; i++) {
      snprintf(label, sizeof label, "Pulse %d", i + 1);
      addPhase(p, label, PhaseType::POWER, piPct, onS);
      used += p.phases[p.phaseCount - 1].durationS;
      if (i < pulses - 1) {
        snprintf(label, sizeof label, "Rest %d", i + 1);
        addPhase(p, label, PhaseType::SOAK, 0.0f, offS);
        used += p.phases[p.phaseCount - 1].durationS;
      }
    }
    if (press) {
      float rampBar = clampf(o["pressure_ramp_bar"] | 9.0f, 0.0f, PHASE_PRESSURE_MAX_BAR);
      addPhase(p, "Ramp", PhaseType::PRESSURE, rampBar, (float)(o["pressure_ramp_ms"] | 20000) / 1000.0f);
      used += p.phases[p.phaseCount - 1].durationS;
      float last = rampBar;
      if (decline) {
        float declineBar = clampf(o["pressure_decline_bar"] | 6.0f, 0.0f, PHASE_PRESSURE_MAX_BAR);
        addPhase(p, "Decline", PhaseType::PRESSURE, declineBar, (float)(o["pressure_decline_ms"] | 8000) / 1000.0f);
        used += p.phases[p.phaseCount - 1].durationS;
        last = declineBar;
      }
      addPhase(p, "Hold", PhaseType::PRESSURE, last, (float)p.maxSec - used);
    } else {
      addPhase(p, "Extract", PhaseType::POWER, (float)clampi(powers.brewPct, 10, 100), (float)p.maxSec - used);
    }

    const char *err = nullptr;
    if (!profileValidate(p, &err)) { (*warnings)++; continue; }
    out[n++] = p;
  }
  return n;
}
