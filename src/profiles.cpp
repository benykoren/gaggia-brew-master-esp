#include "profiles.h"

#include <LittleFS.h>

#include "config.h"

struct ProfileRecord {
  String name;
  double temp;
  unsigned long autoStopSec;
  bool preinfusionEnabled;
  int pulses;
  int onMs;
  int offMs;
};

// Reads every line into memory - PROFILE_MAX_COUNT (8) small CSV rows is a
// trivial amount of RAM/flash I/O, so "load all, modify, rewrite whole file"
// (same approach shot_log.cpp uses for trimming) is simpler than in-place
// line editing and plenty fast enough.
static int loadAll(ProfileRecord out[PROFILE_MAX_COUNT]) {
  int count = 0;
  File f = LittleFS.open(PROFILE_LOG_PATH, "r");
  if (!f) return 0;
  while (f.available() && count < PROFILE_MAX_COUNT) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int c1 = line.indexOf(',');
    int c2 = line.indexOf(',', c1 + 1);
    int c3 = line.indexOf(',', c2 + 1);
    int c4 = line.indexOf(',', c3 + 1);
    int c5 = line.indexOf(',', c4 + 1);
    int c6 = line.indexOf(',', c5 + 1);
    if (c1 < 0 || c2 < 0 || c3 < 0 || c4 < 0 || c5 < 0 || c6 < 0) continue; // malformed, skip

    ProfileRecord &r = out[count];
    r.name = line.substring(0, c1);
    r.temp = line.substring(c1 + 1, c2).toDouble();
    r.autoStopSec = line.substring(c2 + 1, c3).toInt();
    r.preinfusionEnabled = line.substring(c3 + 1, c4).toInt() != 0;
    r.pulses = line.substring(c4 + 1, c5).toInt();
    r.onMs = line.substring(c5 + 1, c6).toInt();
    r.offMs = line.substring(c6 + 1).toInt();
    count++;
  }
  f.close();
  return count;
}

static void saveAll(ProfileRecord recs[], int count) {
  File f = LittleFS.open(PROFILE_LOG_PATH, "w");
  if (!f) {
    Serial.println("profiles: failed to open for write");
    return;
  }
  for (int i = 0; i < count; i++) {
    ProfileRecord &r = recs[i];
    f.printf("%s,%.2f,%lu,%d,%d,%d,%d\n", r.name.c_str(), r.temp, r.autoStopSec,
              r.preinfusionEnabled ? 1 : 0, r.pulses, r.onMs, r.offMs);
  }
  f.close();
}

void profilesInit() {
  if (!LittleFS.begin(true)) {
    Serial.println("profilesInit: LittleFS mount failed even after format");
    return;
  }
  if (LittleFS.exists(PROFILE_LOG_PATH)) return;

  // Seed starter profiles grounded in real community consensus (2026-08-16
  // research pass), not invented numbers - temp-by-roast from Papel
  // Espresso's roast guide, SCA-referenced shot-time windows, and pulsed
  // pre-infusion timings translated from Profitec/Gicar/Gaggiuino roast-
  // tuning guidance (light roasts benefit from a longer low-pressure soak
  // for CO2 degassing/even saturation; dark roasts need little to none).
  // Medium is first/default, matching this project's original Espresso
  // default (93C/27s) and GaggiMate's own default.
  // "Classic" is first/default - straight extraction, no pulsing at all,
  // for anyone who'd rather not have pre-infusion in the mix, or wants a
  // known-plain baseline to compare the others against.
  ProfileRecord seed[7] = {
      {"Classic", 93.0, 27, false, 0, 0, 0},
      {"Medium Roast", 93.0, 27, true, 3, 1000, 2000},
      {"Light Roast", 95.0, 28, true, 4, 1000, 3000},
      {"Dark Roast", 90.0, 26, true, 2, 1000, 1000},
      {"Ristretto", 93.0, 22, true, 2, 1000, 1000},
      {"Lungo", 92.0, 40, true, 3, 1000, 2000},
      {"Decaf", 90.0, 27, true, 3, 1000, 2000},
  };
  saveAll(seed, 7);
  Serial.println("profiles: seeded starter profiles (Classic/Medium/Light/Dark/Ristretto/Lungo/Decaf)");
}

int profileCount() {
  ProfileRecord tmp[PROFILE_MAX_COUNT];
  return loadAll(tmp);
}

String profilesReadJson() {
  ProfileRecord recs[PROFILE_MAX_COUNT];
  int count = loadAll(recs);

  String json = "[";
  for (int i = 0; i < count; i++) {
    if (i > 0) json += ",";
    ProfileRecord &r = recs[i];
    json += "{\"name\":\"" + r.name + "\",\"temp\":" + String(r.temp, 1) +
            ",\"auto_stop_sec\":" + String(r.autoStopSec) +
            ",\"preinfusion\":" + (r.preinfusionEnabled ? "true" : "false") +
            ",\"pulses\":" + String(r.pulses) + ",\"on_ms\":" + String(r.onMs) +
            ",\"off_ms\":" + String(r.offMs) + "}";
  }
  json += "]";
  return json;
}

bool profileGet(int index, String &name, double &temp, unsigned long &autoStopSec,
                bool &preinfusionEnabled, int &pulses, int &onMs, int &offMs) {
  ProfileRecord recs[PROFILE_MAX_COUNT];
  int count = loadAll(recs);
  if (index < 0 || index >= count) return false;
  ProfileRecord &r = recs[index];
  name = r.name;
  temp = r.temp;
  autoStopSec = r.autoStopSec;
  preinfusionEnabled = r.preinfusionEnabled;
  pulses = r.pulses;
  onMs = r.onMs;
  offMs = r.offMs;
  return true;
}

int profileSave(int index, String name, double temp, unsigned long autoStopSec,
                bool preinfusionEnabled, int pulses, int onMs, int offMs) {
  name.replace(",", ""); // CSV storage, no quoting support - keep it simple
  name.replace("\n", "");
  if (name.length() > PROFILE_NAME_MAX_LEN) name = name.substring(0, PROFILE_NAME_MAX_LEN);
  if (name.length() == 0) name = "Profile";

  ProfileRecord recs[PROFILE_MAX_COUNT];
  int count = loadAll(recs);

  ProfileRecord updated = {name, temp, autoStopSec, preinfusionEnabled, pulses, onMs, offMs};

  int targetIndex;
  if (index >= 0 && index < count) {
    targetIndex = index;
    recs[targetIndex] = updated;
  } else {
    if (count >= PROFILE_MAX_COUNT) return -1; // full
    targetIndex = count;
    recs[targetIndex] = updated;
    count++;
  }
  saveAll(recs, count);
  return targetIndex;
}

void profileDelete(int index) {
  ProfileRecord recs[PROFILE_MAX_COUNT];
  int count = loadAll(recs);
  if (index < 0 || index >= count || count <= 1) return; // always keep at least one

  for (int i = index; i < count - 1; i++) recs[i] = recs[i + 1];
  saveAll(recs, count - 1);
}
