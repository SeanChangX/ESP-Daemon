#include "web_server.h"

#include "config.h"
#include "app_settings.h"
#include "espnow_comm.h"
#include "led_control.h"
#include "ros_node.h"
#include "telemetry_log.h"
#include "voltmeter.h"
#include "wifi_config.h"

#include <SPIFFS.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <ElegantOTA.h>

WebServer server(80);

static bool parseBoolString(const String& value, bool& parsed) {
  String normalized = value;
  normalized.toLowerCase();

  if (normalized == "1" || normalized == "true" || normalized == "on") {
    parsed = true;
    return true;
  }
  if (normalized == "0" || normalized == "false" || normalized == "off") {
    parsed = false;
    return true;
  }
  return false;
}

static bool parsePowerControlChannel(const String& key, PowerControlChannel& channel) {
  String normalized = key;
  normalized.toLowerCase();

  if (normalized == "chassis" || normalized == "chassispower") {
    channel = POWER_CHANNEL_CHASSIS;
    return true;
  }
  if (normalized == "mission" || normalized == "missionpower") {
    channel = POWER_CHANNEL_MISSION;
    return true;
  }
  if (normalized == "negativepressure" || normalized == "negativepressurepower" || normalized == "negpressure") {
    channel = POWER_CHANNEL_NEG_PRESSURE;
    return true;
  }
  return false;
}

static void appendPowerState(JsonDocument& payload) {
  payload["chassisPower"]          = getPowerControlState(POWER_CHANNEL_CHASSIS);
  payload["missionPower"]          = getPowerControlState(POWER_CHANNEL_MISSION);
  payload["negativePressurePower"] = getPowerControlState(POWER_CHANNEL_NEG_PRESSURE);
}

static String buildPowerResponse(bool success, const String& message) {
  JsonDocument payload;
  payload["success"] = success;
  payload["message"] = message;
  appendPowerState(payload);
  String output;
  serializeJson(payload, output);
  return output;
}

static String buildSettingsResponse(bool success, const String& message) {
  JsonDocument payload;
  payload["success"] = success;
  payload["message"] = message;
  payload["pinRequired"] = getAppSettings().pin_protection_enabled;
  String output;
  serializeJson(payload, output);
  return output;
}

static String getContentType(const String& path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css"))  return "text/css";
  if (path.endsWith(".js"))   return "application/javascript";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".png"))  return "image/png";
  if (path.endsWith(".ico"))  return "image/x-icon";
  return "text/plain";
}

static String getDeviceNameForMdns() {
  return getAppSettings().device_name;
}

static void handleDeviceInfoGet() {
  JsonDocument doc;
  doc["version"] = ESP_DAEMON_FW_VERSION;

  uint8_t macBytes[6] = {0};
  WiFi.macAddress(macBytes);
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X-%02X-%02X-%02X-%02X-%02X",
           macBytes[0], macBytes[1], macBytes[2], macBytes[3], macBytes[4], macBytes[5]);
  doc["mac"] = macStr;
  doc["controlPanelUrl"] = getAppSettings().control_panel_url;
  doc["settingsPinRequired"] = getAppSettings().pin_protection_enabled;

  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

static bool sendFileFromSPIFFS(const String& path) {
  if (!SPIFFS.exists(path)) {
    return false;
  }

  File file = SPIFFS.open(path, FILE_READ);
  if (!file) {
    return false;
  }

  server.streamFile(file, getContentType(path));
  file.close();
  return true;
}

static bool applyPowerUpdate(const String& controlName, bool enabled) {
  PowerControlChannel channel;
  if (!parsePowerControlChannel(controlName, channel)) {
    return false;
  }
  setPowerControlOverride(channel, enabled);
  return true;
}

static bool parseJsonBody(JsonDocument& jsonObj, String& errorMessage) {
  if (!server.hasArg("plain")) {
    errorMessage = "Missing JSON body";
    return false;
  }

  const DeserializationError error = deserializeJson(jsonObj, server.arg("plain"));
  if (error || !jsonObj.is<JsonObject>()) {
    errorMessage = "Invalid JSON body";
    return false;
  }
  return true;
}

static String jsonTrimmedString(const JsonObjectConst& obj, const char* key) {
  const JsonVariantConst v = obj[key];
  if (v.isNull()) {
    return String();
  }
  String s = v.as<String>();
  s.trim();
  return s;
}

static bool authorizeSettingsRequest(const JsonObjectConst& jsonObj) {
  return verifySettingsPin(jsonTrimmedString(jsonObj, "authPin"));
}

static bool applySettingsUpdate(const JsonObjectConst& updateObj, String& updateError) {
  if (!updateAppSettingsFromJson(updateObj, updateError)) {
    return false;
  }

  refreshNetworkIdentity();
  refreshESPNowChannel();
  if (getAppSettings().runtime_led_enabled) {
    initLED();
  }
  return true;
}

static void handlePowerPost() {
  bool updated = false;

  if (server.hasArg("control") && server.hasArg("enabled")) {
    bool enabled = false;
    if (parseBoolString(server.arg("enabled"), enabled) &&
        applyPowerUpdate(server.arg("control"), enabled)) {
      updated = true;
    }
  }

  if (server.hasArg("chassisPower")) {
    bool enabled = false;
    if (parseBoolString(server.arg("chassisPower"), enabled)) {
      setPowerControlOverride(POWER_CHANNEL_CHASSIS, enabled);
      updated = true;
    }
  }
  if (server.hasArg("missionPower")) {
    bool enabled = false;
    if (parseBoolString(server.arg("missionPower"), enabled)) {
      setPowerControlOverride(POWER_CHANNEL_MISSION, enabled);
      updated = true;
    }
  }
  if (server.hasArg("negativePressurePower")) {
    bool enabled = false;
    if (parseBoolString(server.arg("negativePressurePower"), enabled)) {
      setPowerControlOverride(POWER_CHANNEL_NEG_PRESSURE, enabled);
      updated = true;
    }
  }

  if (server.hasArg("plain")) {
    JsonDocument jsonObj;
    DeserializationError error = deserializeJson(jsonObj, server.arg("plain"));
    if (!error && jsonObj.is<JsonObject>()) {
      if (jsonObj["control"].is<const char*>() && jsonObj["enabled"].is<bool>()) {
        if (applyPowerUpdate(String(jsonObj["control"].as<const char*>()), jsonObj["enabled"].as<bool>())) {
          updated = true;
        }
      }
      if (jsonObj["chassisPower"].is<bool>()) {
        setPowerControlOverride(POWER_CHANNEL_CHASSIS, jsonObj["chassisPower"].as<bool>());
        updated = true;
      }
      if (jsonObj["missionPower"].is<bool>()) {
        setPowerControlOverride(POWER_CHANNEL_MISSION, jsonObj["missionPower"].as<bool>());
        updated = true;
      }
      if (jsonObj["negativePressurePower"].is<bool>()) {
        setPowerControlOverride(POWER_CHANNEL_NEG_PRESSURE, jsonObj["negativePressurePower"].as<bool>());
        updated = true;
      }
    }
  }

  if (updated) {
    server.send(200, "application/json", buildPowerResponse(true, "Power state updated"));
  } else {
    server.send(400, "application/json", buildPowerResponse(false, "Invalid power payload"));
  }
}

static void handleEmergencyPost() {
  bool updated = false;
  bool emergency = false;

  if (server.hasArg("emergency") && parseBoolString(server.arg("emergency"), emergency)) {
    setPowerControlOverride(POWER_CHANNEL_CHASSIS, emergency);
    updated = true;
  }

  if (server.hasArg("plain")) {
    JsonDocument jsonObj;
    DeserializationError error = deserializeJson(jsonObj, server.arg("plain"));
    if (!error && jsonObj["emergency"].is<bool>()) {
      setPowerControlOverride(POWER_CHANNEL_CHASSIS, jsonObj["emergency"].as<bool>());
      updated = true;
    }
  }

  if (updated) {
    server.send(200, "application/json", buildPowerResponse(true, "Chassis power updated"));
  } else {
    server.send(400, "application/json", buildPowerResponse(false, "Invalid emergency payload"));
  }
}

static void handleSettingsReadPost() {
  JsonDocument jsonObj;
  String parseError;
  if (!parseJsonBody(jsonObj, parseError)) {
    server.send(400, "application/json", buildSettingsResponse(false, parseError));
    return;
  }

  if (!authorizeSettingsRequest(jsonObj.as<JsonObjectConst>())) {
    server.send(403, "application/json", buildSettingsResponse(false, "Invalid PIN"));
    return;
  }

  JsonDocument payload;
  appSettingsToJson(payload, false);
  payload["pinRequired"] = getAppSettings().pin_protection_enabled;

  String output;
  serializeJson(payload, output);
  server.send(200, "application/json", output);
}

static void handleSettingsPost() {
  JsonDocument jsonObj;
  String parseError;
  if (!parseJsonBody(jsonObj, parseError)) {
    server.send(400, "application/json", buildSettingsResponse(false, parseError));
    return;
  }

  if (!authorizeSettingsRequest(jsonObj.as<JsonObjectConst>())) {
    server.send(403, "application/json", buildSettingsResponse(false, "Invalid PIN"));
    return;
  }

  String updateError;
  if (!applySettingsUpdate(jsonObj.as<JsonObjectConst>(), updateError)) {
    server.send(400, "application/json", buildSettingsResponse(false, updateError));
    return;
  }

  server.send(200, "application/json", buildSettingsResponse(true, "Settings updated"));
}

static void handleSettingsExportPost() {
  JsonDocument jsonObj;
  String parseError;
  if (!parseJsonBody(jsonObj, parseError)) {
    server.send(400, "application/json", buildSettingsResponse(false, parseError));
    return;
  }

  if (!authorizeSettingsRequest(jsonObj.as<JsonObjectConst>())) {
    server.send(403, "application/json", buildSettingsResponse(false, "Invalid PIN"));
    return;
  }

  JsonDocument payload;
  appSettingsToJson(payload, false);
  payload["pinRequired"] = getAppSettings().pin_protection_enabled;

  String output;
  serializeJsonPretty(payload, output);
  server.sendHeader("Content-Disposition", "attachment; filename=\"settings_export.json\"");
  server.send(200, "application/json", output);
}

static void handleSettingsImportPost() {
  JsonDocument jsonObj;
  String parseError;
  if (!parseJsonBody(jsonObj, parseError)) {
    server.send(400, "application/json", buildSettingsResponse(false, parseError));
    return;
  }

  const JsonObjectConst root = jsonObj.as<JsonObjectConst>();
  if (!authorizeSettingsRequest(root)) {
    server.send(403, "application/json", buildSettingsResponse(false, "Invalid PIN"));
    return;
  }

  const JsonObjectConst updateObj = root["settings"].is<JsonObjectConst>()
    ? root["settings"].as<JsonObjectConst>()
    : root;

  String updateError;
  if (!applySettingsUpdate(updateObj, updateError)) {
    server.send(400, "application/json", buildSettingsResponse(false, updateError));
    return;
  }

  server.send(200, "application/json", buildSettingsResponse(true, "Settings imported"));
}

static void handleSettingsUnlockPost() {
  JsonDocument jsonObj;
  String parseError;
  if (!parseJsonBody(jsonObj, parseError)) {
    server.send(400, "application/json", buildSettingsResponse(false, parseError));
    return;
  }

  if (!getAppSettings().pin_protection_enabled) {
    server.send(200, "application/json", buildSettingsResponse(true, "OK"));
    return;
  }

  const String pin = jsonTrimmedString(jsonObj.as<JsonObjectConst>(), "pin");
  if (verifySettingsPin(pin)) {
    server.send(200, "application/json", buildSettingsResponse(true, "OK"));
    return;
  }

  server.send(403, "application/json", buildSettingsResponse(false, "Invalid PIN"));
}

static void handleSettingsResetPost() {
  JsonDocument jsonObj;
  String parseError;
  if (!parseJsonBody(jsonObj, parseError)) {
    server.send(400, "application/json", buildSettingsResponse(false, parseError));
    return;
  }

  if (!authorizeSettingsRequest(jsonObj.as<JsonObjectConst>())) {
    server.send(403, "application/json", buildSettingsResponse(false, "Invalid PIN"));
    return;
  }

  if (!resetAppSettingsToDefaults()) {
    server.send(400, "application/json", buildSettingsResponse(false, "Failed to reset settings"));
    return;
  }

  refreshNetworkIdentity();
  refreshESPNowChannel();
  if (getAppSettings().runtime_led_enabled) {
    initLED();
  }

  server.send(200, "application/json", buildSettingsResponse(true, "Settings reset to defaults"));
}

static void handleEspRebootPost() {
  JsonDocument jsonObj;
  String parseError;
  if (!parseJsonBody(jsonObj, parseError)) {
    server.send(400, "application/json", buildSettingsResponse(false, parseError));
    return;
  }

  if (!authorizeSettingsRequest(jsonObj.as<JsonObjectConst>())) {
    server.send(403, "application/json", buildSettingsResponse(false, "Invalid PIN"));
    return;
  }

  server.send(200, "application/json", buildSettingsResponse(true, "Rebooting..."));
  delay(50);
  ESP.restart();
}

void initSPIFFS() {
  if (!SPIFFS.begin()) {
    DAEMON_LOGLN("SPIFFS Mount Failed");
  }
}

void initWebServer() {
  server.on("/", HTTP_GET, []() {
    if (!sendFileFromSPIFFS("/index.html")) {
      server.send(404, "text/plain", "index.html not found");
    }
  });

  server.on("/readings", HTTP_GET, []() {
    server.send(200, "application/json", getSensorReadings());
  });

  server.on("/telemetry", HTTP_GET, []() {
    server.send(200, "application/json", telemetryLogGetJson());
  });

  server.on("/health", HTTP_GET, []() {
    server.send(200, "text/plain", "ok");
  });

  server.on("/device", HTTP_GET, handleDeviceInfoGet);

  server.on("/power", HTTP_POST, handlePowerPost);
  server.on("/emergency", HTTP_POST, handleEmergencyPost);
  server.on("/settings", HTTP_GET, []() {
    server.send(405, "application/json", buildSettingsResponse(false, "GET /settings is disabled"));
  });
  server.on("/settings/read", HTTP_POST, handleSettingsReadPost);
  server.on("/settings", HTTP_POST, handleSettingsPost);
  server.on("/settings/export", HTTP_POST, handleSettingsExportPost);
  server.on("/settings/import", HTTP_POST, handleSettingsImportPost);
  server.on("/settings/unlock", HTTP_POST, handleSettingsUnlockPost);
  server.on("/settings/reset", HTTP_POST, handleSettingsResetPost);
  server.on("/esp/reboot", HTTP_POST, handleEspRebootPost);

  server.onNotFound([]() {
    String path = server.uri();

    // Common captive-portal probe endpoints used by phones/OSes.
    if (path == "/generate_204" ||
        path == "/gen_204" ||
        path == "/hotspot-detect.html" ||
        path == "/ncsi.txt" ||
        path == "/connecttest.txt" ||
        path == "/success.txt" ||
        path == "/redirect") {
      server.sendHeader("Location", "/", true);
      server.send(302, "text/plain", "");
      return;
    }

    if (path == "/") {
      path = "/index.html";
    }

    if (sendFileFromSPIFFS(path)) {
      return;
    }

    // Fallback to index for extension-less paths to reduce accidental 404s.
    const int lastSlash = path.lastIndexOf('/');
    const int lastDot = path.lastIndexOf('.');
    const bool hasExtension = (lastDot > lastSlash);
    if (!hasExtension && sendFileFromSPIFFS("/index.html")) {
      return;
    }

    DAEMON_LOGF("HTTP request not found: %s\n", path.c_str());
    server.send(404, "text/plain", "Not found");
  });

  ElegantOTA.begin(&server);
  server.begin();
  const String mdnsHost = getDeviceNameForMdns();
  DAEMON_LOGLN("Web server started on port 80 (sync)");
  DAEMON_LOGF("OTA endpoint: http://%s.local/update\n", mdnsHost.c_str());
}

void handleWebServer() {
  server.handleClient();
  ElegantOTA.loop();
}
