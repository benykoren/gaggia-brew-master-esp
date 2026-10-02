#pragma once

#include <Arduino.h>

#include "profile_types.h"

// Shot profile store on LittleFS (design spec 2026-10-02 §3.1): built-ins
// from firmware (profile_codec.cpp) + up to PROFILE_MAX_COUNT user profiles,
// in PROFILE_STORE_PATH as {"version":1,"user":[...],"builtin_overrides":
// {name:{temp,max_sec,pressure_scale,trim_pct}}}. Everything is addressed by
// NAME (case-insensitive). Call every function with stateMutex held;
// returned pointers are valid until the next mutation.

// Mounts LittleFS, loads built-ins and the store. If the store doesn't
// exist but the legacy PROFILE_LOG_PATH does, backs it up byte-for-byte to
// PROFILE_LEGACY_BACKUP_PATH and migrates it (legacy*Pct = the old global
// pump powers, used for migrated pulse/extract phases).
void profilesInit(int legacyBrewPct, int legacyPreinfusionPct);

int profileCount();                         // built-ins + user
const Profile *profileAt(int i);            // tweaks applied
const Profile *profileFind(const char *name);
String profilesListJson();                  // GET /profiles
String profileLegacyNameAt(int legacyIndex); // old NVS "active_profile" index -> migrated name, "" if none

bool profileTweak(const char *name, const ProfileTweak &t, const char **err);
bool profileDuplicate(const char *src, const char *newName, const char **err);
bool profileRename(const char *oldName, const char *newName, const char **err);
bool profileDeleteUser(const char *name, const char **err);
bool profileReset(const char *name, const char **err);
