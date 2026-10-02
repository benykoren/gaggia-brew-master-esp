#include "profiles.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include "config.h"
#include "profile_codec.h"

static Profile baseBuiltins[PROFILE_BUILTIN_MAX];
static bool builtinTweaked[PROFILE_BUILTIN_MAX];
static ProfileTweak builtinTweaks[PROFILE_BUILTIN_MAX];
static int builtinCount = 0;
static Profile users[PROFILE_MAX_COUNT];
static int userCount = 0;
static Profile effective[PROFILE_BUILTIN_MAX + PROFILE_MAX_COUNT];
static int effectiveCount = 0;
static String legacyNames[PROFILE_MAX_COUNT];
static int legacyCount = 0;

static int findBuiltin(const char *name) {
  for (int i = 0; i < builtinCount; i++)
    if (profileNameEquals(baseBuiltins[i].name, name)) return i;
  return -1;
}

static int findUser(const char *name) {
  for (int i = 0; i < userCount; i++)
    if (profileNameEquals(users[i].name, name)) return i;
  return -1;
}

static void rebuildEffective() {
  effectiveCount = 0;
  for (int i = 0; i < builtinCount; i++) {
    effective[effectiveCount] = baseBuiltins[i];
    if (builtinTweaked[i]) profileApplyTweak(effective[effectiveCount], builtinTweaks[i]);
    effectiveCount++;
  }
  for (int i = 0; i < userCount; i++) effective[effectiveCount++] = users[i];
}

// Write-then-rename so a power cut mid-save can't leave a half-written store.
static bool saveStore() {
  JsonDocument doc;
  doc["version"] = 1;
  JsonArray u = doc["user"].to<JsonArray>();
  for (int i = 0; i < userCount; i++) profileToJson(users[i], u.add<JsonObject>());
  JsonObject ov = doc["builtin_overrides"].to<JsonObject>();
  for (int i = 0; i < builtinCount; i++) {
    if (!builtinTweaked[i]) continue;
    JsonObject t = ov[baseBuiltins[i].name].to<JsonObject>();
    t["temp"] = builtinTweaks[i].temp;
    t["max_sec"] = builtinTweaks[i].maxSec;
    t["pressure_scale"] = builtinTweaks[i].pressureScale;
    t["trim_pct"] = builtinTweaks[i].trimPct;
  }
  const char *tmp = PROFILE_STORE_PATH ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) {
    Serial.println("profiles: can't open temp file for write");
    return false;
  }
  serializeJson(doc, f);
  f.close();
  LittleFS.remove(PROFILE_STORE_PATH);
  if (!LittleFS.rename(tmp, PROFILE_STORE_PATH)) {
    Serial.println("profiles: rename of temp store failed");
    return false;
  }
  return true;
}

static bool loadStore() {
  File f = LittleFS.open(PROFILE_STORE_PATH, "r");
  if (!f) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("profiles: %s unreadable (%s) - moved aside, starting from built-ins\n", PROFILE_STORE_PATH, err.c_str());
    LittleFS.remove(PROFILE_STORE_PATH ".bad");
    LittleFS.rename(PROFILE_STORE_PATH, PROFILE_STORE_PATH ".bad");
    return false;
  }
  for (JsonVariantConst v : doc["user"].as<JsonArrayConst>()) {
    if (userCount >= PROFILE_MAX_COUNT) break;
    const char *e = nullptr;
    Profile p;
    if (!profileFromJson(v.as<JsonObjectConst>(), p, &e)) {
      Serial.printf("profiles: skipped invalid user profile (%s)\n", e);
      continue;
    }
    p.builtin = false;
    users[userCount++] = p;
  }
  for (JsonPairConst kv : doc["builtin_overrides"].as<JsonObjectConst>()) {
    int bi = findBuiltin(kv.key().c_str());
    if (bi < 0) continue;
    JsonObjectConst t = kv.value().as<JsonObjectConst>();
    ProfileTweak tw = profileGetTweak(baseBuiltins[bi]);
    tw.temp = t["temp"] | tw.temp;
    tw.maxSec = t["max_sec"] | tw.maxSec;
    tw.pressureScale = t["pressure_scale"] | tw.pressureScale;
    tw.trimPct = t["trim_pct"] | tw.trimPct;
    builtinTweaks[bi] = tw;
    builtinTweaked[bi] = true;
  }
  return true;
}

static void migrateLegacy(int legacyBrewPct, int legacyPreinfusionPct) {
  if (!LittleFS.exists(PROFILE_LOG_PATH)) return;
  File f = LittleFS.open(PROFILE_LOG_PATH, "r");
  if (!f) return;
  String legacy = f.readString();
  f.close();

  // Backup FIRST, byte for byte - the 2026-08-23 migration lost data with no backup.
  File b = LittleFS.open(PROFILE_LEGACY_BACKUP_PATH, "w");
  if (!b || b.print(legacy) != legacy.length()) {
    if (b) b.close();
    Serial.println("profiles: legacy backup failed - NOT migrating, legacy file left untouched");
    return;
  }
  b.close();

  JsonDocument names;
  if (!deserializeJson(names, legacy)) {
    for (JsonVariantConst v : names.as<JsonArrayConst>()) {
      if (legacyCount >= PROFILE_MAX_COUNT) break;
      legacyNames[legacyCount++] = v["name"] | "";
    }
  }

  int warnings = 0;
  LegacyPowers powers = {legacyBrewPct, legacyPreinfusionPct};
  userCount = migrateLegacyProfiles(legacy.c_str(), powers, baseBuiltins, builtinCount, users, PROFILE_MAX_COUNT, &warnings);
  Serial.printf("profiles: migrated %d legacy profiles (%d warnings); backup at %s\n", userCount, warnings,
                PROFILE_LEGACY_BACKUP_PATH);
}

void profilesInit(int legacyBrewPct, int legacyPreinfusionPct) {
  builtinCount = builtinProfilesLoad(baseBuiltins, PROFILE_BUILTIN_MAX);
  if (builtinCount == 0) Serial.println("profiles: BUILT-IN TABLE FAILED TO PARSE");
  if (!LittleFS.begin(true)) {
    Serial.println("profiles: LittleFS mount failed - built-ins only");
    rebuildEffective();
    return;
  }
  // saveStore() removes the store before renaming the temp into place; a power
  // cut in between leaves only the (complete) .tmp - promote it.
  if (!LittleFS.exists(PROFILE_STORE_PATH) && LittleFS.exists(PROFILE_STORE_PATH ".tmp")) {
    if (LittleFS.rename(PROFILE_STORE_PATH ".tmp", PROFILE_STORE_PATH)) Serial.println("profiles: recovered store from .tmp");
  }
  if (LittleFS.exists(PROFILE_STORE_PATH)) {
    if (!loadStore()) saveStore();
  } else {
    migrateLegacy(legacyBrewPct, legacyPreinfusionPct);
    saveStore();
  }
  rebuildEffective();
}

int profileCount() { return effectiveCount; }
const Profile *profileAt(int i) { return (i >= 0 && i < effectiveCount) ? &effective[i] : nullptr; }

const Profile *profileFind(const char *name) {
  if (!name) return nullptr;
  for (int i = 0; i < effectiveCount; i++)
    if (profileNameEquals(effective[i].name, name)) return &effective[i];
  return nullptr;
}

String profilesListJson() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < effectiveCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    profileToJson(effective[i], o);
    int bi = effective[i].builtin ? findBuiltin(effective[i].name) : -1;
    o["tweaked"] = bi >= 0 && builtinTweaked[bi];
  }
  String out;
  serializeJson(doc, out);
  return out;
}

String profileLegacyNameAt(int legacyIndex) {
  if (legacyIndex < 0 || legacyIndex >= legacyCount) return "";
  const String &n = legacyNames[legacyIndex];
  if (findUser(n.c_str()) >= 0) return n;
  char renamed[PROFILE_NAME_MAX_LEN + 1];
  profileMakeOldName(n.c_str(), renamed, sizeof renamed);
  return findUser(renamed) >= 0 ? String(renamed) : String("");
}

static bool checkNewName(const char *name, const char *except, const char **err) {
  if (!name || !*name) { *err = "Name can't be empty"; return false; }
  if (strlen(name) > PROFILE_NAME_MAX_LEN) { *err = "Name longer than 20 characters"; return false; }
  const Profile *other = profileFind(name);
  if (other && !(except && profileNameEquals(other->name, except))) { *err = "A profile with that name already exists"; return false; }
  return true;
}

static bool commit() {
  rebuildEffective();
  return saveStore();
}

bool profileTweak(const char *name, const ProfileTweak &t, const char **err) {
  int bi = findBuiltin(name);
  if (bi >= 0) {
    Profile tmp = baseBuiltins[bi];
    profileApplyTweak(tmp, t); // clamps
    builtinTweaks[bi] = profileGetTweak(tmp);
    builtinTweaked[bi] = true;
    return commit();
  }
  int ui = findUser(name);
  if (ui < 0) { *err = "No such profile"; return false; }
  profileApplyTweak(users[ui], t);
  return commit();
}

bool profileDuplicate(const char *src, const char *newName, const char **err) {
  const Profile *s = profileFind(src);
  if (!s) { *err = "No such profile"; return false; }
  if (userCount >= PROFILE_MAX_COUNT) { *err = "Profile list is full (8 of your own)"; return false; }
  if (!checkNewName(newName, nullptr, err)) return false;
  Profile p = *s;
  p.builtin = false;
  strncpy(p.name, newName, PROFILE_NAME_MAX_LEN);
  p.name[PROFILE_NAME_MAX_LEN] = '\0';
  users[userCount++] = p;
  return commit();
}

bool profileRename(const char *oldName, const char *newName, const char **err) {
  if (findBuiltin(oldName) >= 0) { *err = "Built-in profiles can't be renamed - duplicate it instead"; return false; }
  int ui = findUser(oldName);
  if (ui < 0) { *err = "No such profile"; return false; }
  if (!checkNewName(newName, users[ui].name, err)) return false;
  strncpy(users[ui].name, newName, PROFILE_NAME_MAX_LEN);
  users[ui].name[PROFILE_NAME_MAX_LEN] = '\0';
  return commit();
}

bool profileDeleteUser(const char *name, const char **err) {
  if (findBuiltin(name) >= 0) { *err = "Built-in profiles can't be deleted"; return false; }
  int ui = findUser(name);
  if (ui < 0) { *err = "No such profile"; return false; }
  for (int i = ui; i < userCount - 1; i++) users[i] = users[i + 1];
  userCount--;
  return commit();
}

bool profileReset(const char *name, const char **err) {
  int bi = findBuiltin(name);
  if (bi < 0) { *err = "Only built-in profiles can be reset"; return false; }
  builtinTweaked[bi] = false;
  return commit();
}
