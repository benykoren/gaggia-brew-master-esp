#pragma once

#include <ArduinoJson.h>
#include <stddef.h>

#include "profile_types.h"

// Pure JSON <-> Profile conversion, validation, the built-in profile table,
// tweak clamping and legacy migration (design spec 2026-10-02 §3). No
// Arduino/LittleFS - the device-side store (profiles.cpp) does the I/O.

// Parses + validates one profile object. On failure returns false and sets
// *err to a static human-readable reason.
bool profileFromJson(JsonObjectConst obj, Profile &out, const char **err);
void profileToJson(const Profile &p, JsonObject obj);
bool profileValidate(const Profile &p, const char **err);

// Parses the firmware's built-in profile table into `out` (builtin = true).
// Returns how many loaded.
int builtinProfilesLoad(Profile *out, int maxOut);

ProfileTweak profileGetTweak(const Profile &p);
// Applies a tweak, clamping every field into range (NaN -> the minimum).
// Temperature is ignored for utility profiles.
void profileApplyTweak(Profile &p, const ProfileTweak &t);

bool profileNameEquals(const char *a, const char *b); // case-insensitive
// "<in> (old)", with `in` truncated so the result fits PROFILE_NAME_MAX_LEN.
void profileMakeOldName(const char *in, char *out, size_t cap);

struct LegacyPowers {
  int brewPct;        // old global "pump_brew_pct"
  int preinfusionPct; // old global "pump_pi_pct"
};
// Converts the pre-2026-10-02 flat JSON array (still stored at
// PROFILE_LOG_PATH) into phase profiles. Names that collide (case-
// insensitively) with `taken` or with an earlier migrated profile get
// " (old)". Invalid entries are skipped. *warnings counts skipped entries
// and capped pulse counts. Returns the number written to `out`.
int migrateLegacyProfiles(const char *legacyJson, const LegacyPowers &powers, const Profile *taken,
                          int takenCount, Profile *out, int maxOut, int *warnings);

const char *phaseTypeName(PhaseType t);
