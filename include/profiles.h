#pragma once

#include <Arduino.h>

// Named shot profiles on LittleFS (one CSV line each) - see config.h for the
// storage rationale and full pre-infusion reasoning. Mirrors shot_log.h's
// pattern: small, bounded, rewrite-the-whole-file-on-change (trivial at
// PROFILE_MAX_COUNT's scale).

// Mounts LittleFS (safe to call even if shotLogInit() already did) and seeds
// three starter profiles (Espresso/Ristretto/Lungo) if the file doesn't
// exist yet, so upgrading from the old preset system isn't a blank slate.
void profilesInit();

// Returns the full profile list as a JSON array, e.g.:
// [{"name":"Espresso","temp":93.0,"auto_stop_sec":27,"preinfusion":true,
//   "pulses":4,"on_ms":1000,"off_ms":1000}, ...]
String profilesReadJson();

int profileCount();

// Loads profile `index`'s fields into the out-params. Returns false (params
// untouched) if index is out of range.
bool profileGet(int index, String &name, double &temp, unsigned long &autoStopSec,
                bool &preinfusionEnabled, int &pulses, int &onMs, int &offMs);

// Adds a new profile (index == -1 or >= profileCount()) or overwrites an
// existing one. Returns the resulting index, or -1 if the list is already
// at PROFILE_MAX_COUNT and index requested a new entry. Commas in `name`
// are stripped (CSV storage, no quoting support - keeps this simple).
int profileSave(int index, String name, double temp, unsigned long autoStopSec,
                bool preinfusionEnabled, int pulses, int onMs, int offMs);

// Removes profile `index`, shifting later indices down by one. No-op if out
// of range or it would empty the list entirely (always keep at least one).
void profileDelete(int index);
