#include "mqtt.h"
#include "config.h"
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>

// ============================================================================
// MQTT / Home Assistant (rewritten 2026-08-16)
// ----------------------------------------------------------------------------
// The previous version of this file predated the Brew/Steam mode split
// (config.h/main.cpp): its "gaggia/set/mode" handler called myPID.SetMode()
// directly instead of setOpMode(), bypassing gain-scheduling, activeMaxSafety,
// and every safety fix since built into the OFF-mode gate in main.cpp's
// loop() - exactly the class of bug just fixed for the Web UI/loop paths, via
// a different path MQTT never went through. Its Kp/Ki/Kd/target handlers
// also persisted to NVS keys ("kp"/"ki"/"kd"/"target") that main.cpp's
// setup() never reads back, so any MQTT-driven tuning change was silently
// lost on reboot, and reverted moments later at runtime by the next call to
// applyActiveProfile() (shot start/stop, eco-sleep, any Web UI edit).
//
// Every command below now routes through the same setOpMode()/
// refreshActiveProfileIfChanged() API and the same NVS keys the Web UI uses,
// so MQTT/HA and the Web UI can never silently disagree with each other.
// steamMaxSafety is deliberately NOT exposed here - a safety ceiling
// shouldn't be one HA automation mistake away from being raised.
// ============================================================================

extern OpMode currentMode;
extern double brewSetpoint, brewKp, brewKi, brewKd;
extern double steamSetpoint, steamKp, steamKi, steamKd;
extern float currentTemperature;
extern double Output;
extern bool sensorFault;
extern bool shotInProgress;
extern unsigned long shotCount;
extern unsigned long descaleShotThreshold;
extern unsigned long descaleDayThreshold;
extern time_t lastDescaleTime;

extern void setOpMode(OpMode mode);
extern void refreshActiveProfileIfChanged();
extern void noteActivity();
extern void stopAutotune();

// See config.h "Shared-state lock" - callback() runs on the default Arduino
// loop task, a different task than controlTick(), so it needs the same lock
// controlTick() and web.cpp's /update handler hold for their own bodies.
extern void lockState();
extern void unlockState();

WiFiClient espClient;
PubSubClient client(espClient);

String mqtt_server = "";
int mqtt_port = 1883;
String mqtt_user = "";
String mqtt_pass = "";

unsigned long lastMsg = 0;
const long interval = 2000; // Publish every 2s

static void saveDouble(const char *key, double v) {
  Preferences preferences;
  preferences.begin("gaggia", false);
  preferences.putDouble(key, v);
  preferences.end();
}

void callback(char *topic, byte *payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.printf("MQTT message [%s] %s\n", topic, message.c_str());
  String t = String(topic);
  noteActivity(); // an MQTT command is an explicit user/automation action,
                   // same as any Web UI /update - keeps eco-sleep in sync.

  // See config.h "Shared-state lock" - held for the whole handler (matching
  // web.cpp's /update) so a mode/tuning change from MQTT can never
  // interleave with a half-finished control tick.
  lockState();

  if (t == "gaggia/set/mode") {
    stopAutotune(); // mirror web.cpp: a mode change always interrupts autotune
    if (message == "off") setOpMode(OpMode::OFF);
    else if (message == "brew") setOpMode(OpMode::BREW);
    else if (message == "steam") setOpMode(OpMode::STEAM);
    unlockState();
    return;
  }

  bool touchedBrew = false, touchedSteam = false;
  if (t == "gaggia/set/brew_target") { brewSetpoint = message.toDouble(); saveDouble("brew_target", brewSetpoint); touchedBrew = true; }
  else if (t == "gaggia/set/brew_kp") { brewKp = message.toDouble(); saveDouble("brew_kp", brewKp); touchedBrew = true; }
  else if (t == "gaggia/set/brew_ki") { brewKi = message.toDouble(); saveDouble("brew_ki", brewKi); touchedBrew = true; }
  else if (t == "gaggia/set/brew_kd") { brewKd = message.toDouble(); saveDouble("brew_kd", brewKd); touchedBrew = true; }
  else if (t == "gaggia/set/steam_target") { steamSetpoint = message.toDouble(); saveDouble("steam_target", steamSetpoint); touchedSteam = true; }
  else if (t == "gaggia/set/steam_kp") { steamKp = message.toDouble(); saveDouble("steam_kp", steamKp); touchedSteam = true; }
  else if (t == "gaggia/set/steam_ki") { steamKi = message.toDouble(); saveDouble("steam_ki", steamKi); touchedSteam = true; }
  else if (t == "gaggia/set/steam_kd") { steamKd = message.toDouble(); saveDouble("steam_kd", steamKd); touchedSteam = true; }

  if (touchedBrew || touchedSteam) {
    // Applies live only if the touched profile is the one currently active -
    // same behavior as web.cpp's /update handler for tuning-field edits.
    refreshActiveProfileIfChanged();
  }

  unlockState();
}

static String haDevice() {
  return "\"dev\":{"
         "\"ids\":[\"gaggiabrewmasteresp\"],"
         "\"name\":\"GaggiaBrewMasterESP\","
         "\"mdl\":\"ESP32-S3 PID Controller\","
         "\"mf\":\"DIY\","
         "\"sw\":\"" + String(FIRMWARE_BUILD_TIMESTAMP) + "\"}";
}

static void publishDiscovery(const String &component, const String &objectId,
                             const String &config) {
  String topic = "homeassistant/" + component + "/gaggiabrewmasteresp/" +
                 objectId + "/config";
  client.publish(topic.c_str(), config.c_str(), true);
}

// Every entity references this so HA shows "unavailable" instead of a stale
// last value across a reboot/WiFi drop - set via MQTT Last Will (reconnect())
// and republished "online" (retained) right after a successful connect.
static const char *AVAILABILITY_TOPIC = "gaggia/availability";

void sendDiscovery() {
  String dev = haDevice();
  String avail = "\"avty_t\":\"" + String(AVAILABILITY_TOPIC) + "\",";

  // Mode select - the one authoritative way to change Off/Brew/Steam over
  // MQTT; deliberately separate from a climate entity's mode so there's no
  // ambiguity about what "heat" would mean with two non-off modes.
  publishDiscovery("select", "mode",
      "{" + avail + "\"name\":\"Mode\",\"uniq_id\":\"gaggia_mode\","
      "\"cmd_t\":\"gaggia/set/mode\",\"stat_t\":\"gaggia/status\","
      "\"val_tpl\":\"{{ value_json.opmode }}\","
      "\"options\":[\"off\",\"brew\",\"steam\"],\"icon\":\"mdi:coffee-maker\"," + dev + "}");

  // Live readings
  publishDiscovery("sensor", "temp",
      "{" + avail + "\"name\":\"Temperature\",\"uniq_id\":\"gaggia_temp\","
      "\"stat_t\":\"gaggia/status\",\"val_tpl\":\"{{ value_json.temp }}\","
      "\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\"," + dev + "}");
  publishDiscovery("sensor", "output",
      "{" + avail + "\"name\":\"Heater Output\",\"uniq_id\":\"gaggia_output\","
      "\"stat_t\":\"gaggia/status\",\"val_tpl\":\"{{ value_json.output }}\","
      "\"unit_of_meas\":\"%\"," + dev + "}");
  publishDiscovery("binary_sensor", "shot",
      "{" + avail + "\"name\":\"Shot In Progress\",\"uniq_id\":\"gaggia_shot\","
      "\"stat_t\":\"gaggia/status\",\"val_tpl\":\"{{ 'ON' if value_json.shot_in_progress else 'OFF' }}\"," + dev + "}");
  publishDiscovery("binary_sensor", "fault",
      "{" + avail + "\"name\":\"Sensor Fault\",\"uniq_id\":\"gaggia_fault\",\"dev_cla\":\"problem\","
      "\"stat_t\":\"gaggia/status\",\"val_tpl\":\"{{ 'ON' if value_json.fault else 'OFF' }}\"," + dev + "}");
  publishDiscovery("binary_sensor", "descale",
      "{" + avail + "\"name\":\"Descale Due\",\"uniq_id\":\"gaggia_descale\",\"dev_cla\":\"problem\","
      "\"stat_t\":\"gaggia/status\",\"val_tpl\":\"{{ 'ON' if value_json.descale_due else 'OFF' }}\"," + dev + "}");

  // Brew profile numbers
  publishDiscovery("number", "brew_target",
      "{" + avail + "\"name\":\"Brew Target\",\"uniq_id\":\"gaggia_brew_target\","
      "\"cmd_t\":\"gaggia/set/brew_target\",\"stat_t\":\"gaggia/status\","
      "\"val_tpl\":\"{{ value_json.brew_target }}\",\"min\":80,\"max\":100,\"step\":0.5," + dev + "}");
  publishDiscovery("number", "brew_kp",
      "{" + avail + "\"name\":\"Brew Kp\",\"uniq_id\":\"gaggia_brew_kp\","
      "\"cmd_t\":\"gaggia/set/brew_kp\",\"stat_t\":\"gaggia/status\","
      "\"val_tpl\":\"{{ value_json.brew_kp }}\",\"min\":0,\"max\":100,\"step\":0.1," + dev + "}");
  publishDiscovery("number", "brew_ki",
      "{" + avail + "\"name\":\"Brew Ki\",\"uniq_id\":\"gaggia_brew_ki\","
      "\"cmd_t\":\"gaggia/set/brew_ki\",\"stat_t\":\"gaggia/status\","
      "\"val_tpl\":\"{{ value_json.brew_ki }}\",\"min\":0,\"max\":10,\"step\":0.01," + dev + "}");
  publishDiscovery("number", "brew_kd",
      "{" + avail + "\"name\":\"Brew Kd\",\"uniq_id\":\"gaggia_brew_kd\","
      "\"cmd_t\":\"gaggia/set/brew_kd\",\"stat_t\":\"gaggia/status\","
      "\"val_tpl\":\"{{ value_json.brew_kd }}\",\"min\":0,\"max\":50,\"step\":0.1," + dev + "}");

  // Steam profile numbers (steamMaxSafety intentionally not exposed - see
  // file header)
  publishDiscovery("number", "steam_target",
      "{" + avail + "\"name\":\"Steam Target\",\"uniq_id\":\"gaggia_steam_target\","
      "\"cmd_t\":\"gaggia/set/steam_target\",\"stat_t\":\"gaggia/status\","
      "\"val_tpl\":\"{{ value_json.steam_target }}\",\"min\":100,\"max\":130,\"step\":0.5," + dev + "}");
  publishDiscovery("number", "steam_kp",
      "{" + avail + "\"name\":\"Steam Kp\",\"uniq_id\":\"gaggia_steam_kp\","
      "\"cmd_t\":\"gaggia/set/steam_kp\",\"stat_t\":\"gaggia/status\","
      "\"val_tpl\":\"{{ value_json.steam_kp }}\",\"min\":0,\"max\":100,\"step\":0.1," + dev + "}");
  publishDiscovery("number", "steam_ki",
      "{" + avail + "\"name\":\"Steam Ki\",\"uniq_id\":\"gaggia_steam_ki\","
      "\"cmd_t\":\"gaggia/set/steam_ki\",\"stat_t\":\"gaggia/status\","
      "\"val_tpl\":\"{{ value_json.steam_ki }}\",\"min\":0,\"max\":10,\"step\":0.01," + dev + "}");
  publishDiscovery("number", "steam_kd",
      "{" + avail + "\"name\":\"Steam Kd\",\"uniq_id\":\"gaggia_steam_kd\","
      "\"cmd_t\":\"gaggia/set/steam_kd\",\"stat_t\":\"gaggia/status\","
      "\"val_tpl\":\"{{ value_json.steam_kd }}\",\"min\":0,\"max\":50,\"step\":0.1," + dev + "}");
}

void reconnect() {
  if (mqtt_server == "") return;
  if (!client.connected()) {
    Serial.printf("Attempting MQTT connection to %s:%d\n", mqtt_server.c_str(), mqtt_port);

    String clientId = "GaggiaBrewMasterESP-" + String(random(0xffff), HEX);

    // Last Will: broker marks us "offline" (retained) automatically if the
    // connection drops without a clean disconnect (crash, WiFi loss, power
    // cut) - every discovered entity's avty_t points at this same topic.
    bool connected = client.connect(clientId.c_str(),
        mqtt_user.length() > 0 ? mqtt_user.c_str() : nullptr,
        mqtt_user.length() > 0 ? mqtt_pass.c_str() : nullptr,
        AVAILABILITY_TOPIC, 0, true, "offline");

    if (connected) {
      Serial.println("MQTT: Connected!");
      client.subscribe("gaggia/set/#");
      client.publish(AVAILABILITY_TOPIC, "online", true);
      sendDiscovery();
    } else {
      Serial.printf("MQTT: Failed, rc=%d, retrying in 5s\n", client.state());
    }
  }
}

void setupMQTT() {
  Preferences preferences;
  preferences.begin("gaggia", true);
  mqtt_server = preferences.getString("mqtt_server", "");
  mqtt_port = preferences.getInt("mqtt_port", 1883);
  mqtt_user = preferences.getString("mqtt_user", "");
  mqtt_pass = preferences.getString("mqtt_pass", "");
  preferences.end();

  Serial.printf("MQTT config: server=%s port=%d user=%s\n", mqtt_server.c_str(),
                mqtt_port, mqtt_user.c_str());

  if (mqtt_server != "") {
    client.setServer(mqtt_server.c_str(), mqtt_port);
    client.setCallback(callback);
    client.setBufferSize(1024); // HA discovery payloads are larger than the 256B default
  }
}

void publishStatus() {
  if (mqtt_server == "" || !client.connected()) return;

  // Snapshot under the lock first (see config.h "Shared-state lock"), then
  // build/publish the JSON unlocked - same pattern as web.cpp's /status.
  lockState();
  float snapTemp = currentTemperature;
  double snapOutput = Output;
  OpMode snapMode = currentMode;
  double snapBrewTarget = brewSetpoint, snapBrewKp = brewKp, snapBrewKi = brewKi, snapBrewKd = brewKd;
  double snapSteamTarget = steamSetpoint, snapSteamKp = steamKp, snapSteamKi = steamKi, snapSteamKd = steamKd;
  bool snapShotInProgress = shotInProgress;
  bool snapFault = sensorFault;
  unsigned long snapShotCount = shotCount;
  time_t snapLastDescaleTime = lastDescaleTime;
  unsigned long snapDescaleShotThreshold = descaleShotThreshold;
  unsigned long snapDescaleDayThreshold = descaleDayThreshold;
  unlockState();

  bool descaleDue = (snapShotCount >= snapDescaleShotThreshold);
  if (!descaleDue && snapLastDescaleTime > 0) {
    long daysSince = (long)((time(nullptr) - snapLastDescaleTime) / 86400L);
    if (daysSince >= 0 && (unsigned long)daysSince >= snapDescaleDayThreshold) descaleDue = true;
  }

  String json = "{";
  json += "\"temp\":" + String(snapTemp);
  json += ",\"output\":" + String(snapOutput / 10.0); // 0-1000 window -> 0-100%
  json += ",\"opmode\":\"";
  json += (snapMode == OpMode::BREW) ? "brew" : (snapMode == OpMode::STEAM) ? "steam" : "off";
  json += "\"";
  json += ",\"brew_target\":" + String(snapBrewTarget);
  json += ",\"brew_kp\":" + String(snapBrewKp, 4);
  json += ",\"brew_ki\":" + String(snapBrewKi, 4);
  json += ",\"brew_kd\":" + String(snapBrewKd, 4);
  json += ",\"steam_target\":" + String(snapSteamTarget);
  json += ",\"steam_kp\":" + String(snapSteamKp, 4);
  json += ",\"steam_ki\":" + String(snapSteamKi, 4);
  json += ",\"steam_kd\":" + String(snapSteamKd, 4);
  json += ",\"shot_in_progress\":" + String(snapShotInProgress ? "true" : "false");
  json += ",\"fault\":" + String(snapFault ? "true" : "false");
  json += ",\"descale_due\":" + String(descaleDue ? "true" : "false");
  json += "}";

  client.publish("gaggia/status", json.c_str());
}

void handleMQTT() {
  if (mqtt_server == "") return;

  if (!client.connected()) {
    static unsigned long lastReconnectAttempt = 0;
    long now = millis();
    if (now - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = now;
      reconnect();
    }
  } else {
    client.loop();
    unsigned long now = millis();
    if (now - lastMsg > interval) {
      lastMsg = now;
      publishStatus();
    }
  }
}
