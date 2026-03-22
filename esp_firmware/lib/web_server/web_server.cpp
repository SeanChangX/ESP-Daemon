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
#if APP_MODE == APP_MODE_ESTOP
  payload["pinRequired"] = false;
#else
  payload["pinRequired"] = getAppSettings().pin_protection_enabled;
#endif
  String output;
  serializeJson(payload, output);
  return output;
}

static void redirectToRoot() {
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
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

#if APP_MODE == APP_MODE_ESTOP
static bool isCrossModeUiAssetPath(const String& path) {
  return path == "/index.html" ||
         path == "/script.js" ||
         path == "/settings.html" ||
         path == "/settings.js";
}
#else
static bool isCrossModeUiAssetPath(const String& path) {
  return path == "/estop.html" ||
         path == "/estop.js";
}
#endif

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
#if APP_MODE == APP_MODE_ESTOP
  doc["settingsPinRequired"] = false;
#else
  doc["settingsPinRequired"] = getAppSettings().pin_protection_enabled;
#endif

  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

static void handleEStopStatusGet() {
  const AppSettings& settings = getAppSettings();

  JsonDocument doc;
  doc["pressed"]                 = isEStopSwitchPressed();
  doc["rawLevel"]                = getEStopSwitchRawLevel();
  doc["switchPin"]               = settings.estop_switch_pin;
  doc["switchActiveHigh"]        = settings.estop_switch_active_high;
  doc["switchLogicInverted"]     = settings.estop_switch_logic_inverted;
  doc["targetMac"]               = settings.estop_target_mac;
  doc["targetPeerConfigured"]    = isEStopPeerConfigured();
  doc["effectiveTargetMac"]      = getEStopTargetMac();
  doc["espNowChannel"]           = wifi_channel;
  doc["packetsSent"]             = getEStopPacketCount();
  doc["wledStatus"]              = getEStopWledStatus();
  doc["wledSnapshotReady"]       = isEStopWledSnapshotReady();
  doc["routeCount"]              = settings.estop_routes.size();
  doc["pressedRouteCount"]       = getEStopPressedRouteCount();
  doc["configuredPeerCount"]     = getEStopConfiguredPeerCount();

  JsonArray routes = doc["routes"].to<JsonArray>();
  const size_t routeCount = settings.estop_routes.size();
  for (size_t i = 0; i < routeCount; ++i) {
    JsonObject route = routes.add<JsonObject>();
    route["index"] = i;
    route["switchPin"] = settings.estop_routes[i].switch_pin;
    route["switchActiveHigh"] = settings.estop_routes[i].switch_active_high;
    route["switchLogicInverted"] = settings.estop_routes[i].switch_logic_inverted;
    route["targetMac"] = settings.estop_routes[i].target_mac;
    route["pressed"] = isEStopRoutePressed(i);
    route["rawLevel"] = getEStopRouteRawLevel(i);
    route["peerConfigured"] = isEStopRoutePeerConfigured(i);
    route["effectiveTargetMac"] = getEStopRouteEffectiveTargetMac(i);
  }

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
#if APP_MODE == APP_MODE_ESTOP
  (void)jsonObj;
  return true;
#else
  return verifySettingsPin(jsonTrimmedString(jsonObj, "authPin"));
#endif
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

static void copyKeyIfPresent(const JsonObjectConst& src, JsonObject& dst, const char* key) {
  const JsonVariantConst v = src[key];
  if (v.isNull()) {
    return;
  }
  dst[key] = v;
}

static void copyMappedKeyIfPresent(const JsonObjectConst& src, JsonObject& dst, const char* srcKey, const char* dstKey) {
  const JsonVariantConst v = src[srcKey];
  if (v.isNull()) {
    return;
  }
  dst[dstKey] = v;
}

static bool looksStructuredSettingsPayload(const JsonObjectConst& src) {
  return src["device"].is<JsonObjectConst>() ||
         src["runtime"].is<JsonObjectConst>() ||
         src["ros"].is<JsonObjectConst>() ||
         src["io"].is<JsonObjectConst>() ||
         src["led"].is<JsonObjectConst>() ||
         src["voltmeter"].is<JsonObjectConst>() ||
         src["network"].is<JsonObjectConst>() ||
         src["estop"].is<JsonObjectConst>();
}

static bool validateStructuredSettingsPayload(const JsonObjectConst& src, String& error) {
  if (!src["schema"].is<const char*>()) {
    error = "Unsupported import format: missing schema";
    return false;
  }
  const String schema = String(src["schema"].as<const char*>());
  if (schema != "esp-daemon.settings-export") {
    error = "Unsupported import schema";
    return false;
  }

  if (!src["schemaVersion"].is<int>()) {
    error = "Unsupported import format: missing schemaVersion";
    return false;
  }
  const int schemaVersion = src["schemaVersion"].as<int>();
  if (schemaVersion != 2) {
    error = "Unsupported schemaVersion (expected 2)";
    return false;
  }

  if (!src["format"].is<const char*>()) {
    error = "Unsupported import format: missing format";
    return false;
  }
  const String format = String(src["format"].as<const char*>());
  if (format != "structured") {
    error = "Unsupported import format (expected structured)";
    return false;
  }

  if (!looksStructuredSettingsPayload(src)) {
    error = "Unsupported import payload: missing structured sections";
    return false;
  }

  return true;
}

static void buildStructuredSettingsExportPayload(JsonDocument& payload) {
  JsonDocument flat;
  appSettingsToJson(flat, false);
  flat.remove("pinProtectionEnabled");
  flat.remove("pinCode");
  flat.remove("pinRequired");

  const JsonObjectConst src = flat.as<JsonObjectConst>();

  payload["schema"]         = "esp-daemon.settings-export";
  payload["schemaVersion"]  = 2;
  payload["format"]         = "structured";
  payload["generatedBy"]    = "esp-daemon";
  payload["fwVersion"]      = ESP_DAEMON_FW_VERSION;
  payload["exportedAtMs"]   = millis();

  JsonObject device = payload["device"].to<JsonObject>();
  copyMappedKeyIfPresent(src, device, "deviceName", "name");
  copyKeyIfPresent(src, device, "controlPanelUrl");

  JsonObject runtime = payload["runtime"].to<JsonObject>();
  copyMappedKeyIfPresent(src, runtime, "runtimeEspNowEnabled",   "espNowEnabled");
  copyMappedKeyIfPresent(src, runtime, "runtimeMicroRosEnabled", "microRosEnabled");
  copyMappedKeyIfPresent(src, runtime, "runtimeLedEnabled",      "ledEnabled");
  copyMappedKeyIfPresent(src, runtime, "runtimeSensorEnabled",   "sensorEnabled");

  JsonObject ros = payload["ros"].to<JsonObject>();
  copyMappedKeyIfPresent(src, ros, "rosNodeName",        "nodeName");
  copyMappedKeyIfPresent(src, ros, "rosDomainId",        "domainId");
  copyMappedKeyIfPresent(src, ros, "rosTimerMs",         "timerMs");
  copyMappedKeyIfPresent(src, ros, "mrosTimeoutMs",      "timeoutMs");
  copyMappedKeyIfPresent(src, ros, "mrosPingIntervalMs", "pingIntervalMs");

  JsonObject io = payload["io"].to<JsonObject>();
  JsonObject ioSwitches = io["switches"].to<JsonObject>();
  copyMappedKeyIfPresent(src, ioSwitches, "chassisSwitchPin",     "chassisPin");
  copyMappedKeyIfPresent(src, ioSwitches, "missionSwitchPin",     "missionPin");
  copyMappedKeyIfPresent(src, ioSwitches, "negPressureSwitchPin", "negativePressurePin");

  JsonObject ioPower = io["powerOutputs"].to<JsonObject>();
  copyMappedKeyIfPresent(src, ioPower, "chassisPowerPin",     "chassisPin");
  copyMappedKeyIfPresent(src, ioPower, "missionPower12vPin",  "mission12vPin");
  copyMappedKeyIfPresent(src, ioPower, "missionPower7v4Pin",  "mission7v4Pin");
  copyMappedKeyIfPresent(src, ioPower, "negPressurePowerPin", "negativePressurePin");

  JsonObject ioLogic = io["logic"].to<JsonObject>();
  copyKeyIfPresent(src, ioLogic, "switchActiveHigh");
  copyKeyIfPresent(src, ioLogic, "powerActiveHigh");

  JsonObject led = payload["led"].to<JsonObject>();
  copyKeyIfPresent(src, led, "ledPin");
  copyKeyIfPresent(src, led, "ledCount");
  copyKeyIfPresent(src, led, "ledBrightness");
  copyKeyIfPresent(src, led, "ledOverrideDurationMs");

  JsonObject voltmeter = payload["voltmeter"].to<JsonObject>();
  copyKeyIfPresent(src, voltmeter, "voltmeterPin");
  copyKeyIfPresent(src, voltmeter, "voltageDividerR1");
  copyKeyIfPresent(src, voltmeter, "voltageDividerR2");
  copyKeyIfPresent(src, voltmeter, "voltmeterCalibration");
  copyKeyIfPresent(src, voltmeter, "voltmeterOffset");
  copyKeyIfPresent(src, voltmeter, "slidingWindowSize");
  copyKeyIfPresent(src, voltmeter, "timerPeriodUs");
  copyKeyIfPresent(src, voltmeter, "batteryDisconnectThreshold");
  copyKeyIfPresent(src, voltmeter, "batteryLowThreshold");

  JsonObject network = payload["network"].to<JsonObject>();
  copyKeyIfPresent(src, network, "espNowChannel");
  copyKeyIfPresent(src, network, "emergencySwitchMacs");

  JsonObject estop = payload["estop"].to<JsonObject>();
  copyMappedKeyIfPresent(src, estop, "estopTargetMac",           "targetMac");
  copyMappedKeyIfPresent(src, estop, "estopSwitchPin",           "switchPin");
  copyMappedKeyIfPresent(src, estop, "estopSwitchActiveHigh",    "switchActiveHigh");
  copyMappedKeyIfPresent(src, estop, "estopSwitchLogicInverted", "switchLogicInverted");
  copyMappedKeyIfPresent(src, estop, "estopRoutes",              "routes");

  JsonObject estopWled = estop["wled"].to<JsonObject>();
  copyMappedKeyIfPresent(src, estopWled, "estopWledEnabled", "enabled");
  copyMappedKeyIfPresent(src, estopWled, "estopWledBaseUrl", "baseUrl");
  copyMappedKeyIfPresent(src, estopWled, "estopWledPreset",  "preset");

  JsonObject estopBuzzer = estop["buzzer"].to<JsonObject>();
  copyMappedKeyIfPresent(src, estopBuzzer, "estopBuzzerEnabled", "enabled");
  copyMappedKeyIfPresent(src, estopBuzzer, "estopBuzzerPin",     "pin");
}

static void normalizeStructuredSettingsForImport(const JsonObjectConst& src, JsonObject& dst) {
  if (src["device"].is<JsonObjectConst>()) {
    const JsonObjectConst device = src["device"].as<JsonObjectConst>();
    copyMappedKeyIfPresent(device, dst, "name", "deviceName");
    copyKeyIfPresent(device, dst, "controlPanelUrl");
  }

  if (src["runtime"].is<JsonObjectConst>()) {
    const JsonObjectConst runtime = src["runtime"].as<JsonObjectConst>();
    copyMappedKeyIfPresent(runtime, dst, "espNowEnabled",   "runtimeEspNowEnabled");
    copyMappedKeyIfPresent(runtime, dst, "microRosEnabled", "runtimeMicroRosEnabled");
    copyMappedKeyIfPresent(runtime, dst, "ledEnabled",      "runtimeLedEnabled");
    copyMappedKeyIfPresent(runtime, dst, "sensorEnabled",   "runtimeSensorEnabled");
  }

  if (src["ros"].is<JsonObjectConst>()) {
    const JsonObjectConst ros = src["ros"].as<JsonObjectConst>();
    copyMappedKeyIfPresent(ros, dst, "nodeName",       "rosNodeName");
    copyMappedKeyIfPresent(ros, dst, "domainId",       "rosDomainId");
    copyMappedKeyIfPresent(ros, dst, "timerMs",        "rosTimerMs");
    copyMappedKeyIfPresent(ros, dst, "timeoutMs",      "mrosTimeoutMs");
    copyMappedKeyIfPresent(ros, dst, "pingIntervalMs", "mrosPingIntervalMs");
  }

  if (src["io"].is<JsonObjectConst>()) {
    const JsonObjectConst io = src["io"].as<JsonObjectConst>();
    if (io["switches"].is<JsonObjectConst>()) {
      const JsonObjectConst ioSwitches = io["switches"].as<JsonObjectConst>();
      copyMappedKeyIfPresent(ioSwitches, dst, "chassisPin",          "chassisSwitchPin");
      copyMappedKeyIfPresent(ioSwitches, dst, "missionPin",          "missionSwitchPin");
      copyMappedKeyIfPresent(ioSwitches, dst, "negativePressurePin", "negPressureSwitchPin");
    }
    if (io["powerOutputs"].is<JsonObjectConst>()) {
      const JsonObjectConst ioPower = io["powerOutputs"].as<JsonObjectConst>();
      copyMappedKeyIfPresent(ioPower, dst, "chassisPin",          "chassisPowerPin");
      copyMappedKeyIfPresent(ioPower, dst, "mission12vPin",       "missionPower12vPin");
      copyMappedKeyIfPresent(ioPower, dst, "mission7v4Pin",       "missionPower7v4Pin");
      copyMappedKeyIfPresent(ioPower, dst, "negativePressurePin", "negPressurePowerPin");
    }
    if (io["logic"].is<JsonObjectConst>()) {
      const JsonObjectConst ioLogic = io["logic"].as<JsonObjectConst>();
      copyKeyIfPresent(ioLogic, dst, "switchActiveHigh");
      copyKeyIfPresent(ioLogic, dst, "powerActiveHigh");
    }
  }

  if (src["led"].is<JsonObjectConst>()) {
    const JsonObjectConst led = src["led"].as<JsonObjectConst>();
    copyKeyIfPresent(led, dst, "ledPin");
    copyKeyIfPresent(led, dst, "ledCount");
    copyKeyIfPresent(led, dst, "ledBrightness");
    copyKeyIfPresent(led, dst, "ledOverrideDurationMs");
  }

  if (src["voltmeter"].is<JsonObjectConst>()) {
    const JsonObjectConst voltmeter = src["voltmeter"].as<JsonObjectConst>();
    copyKeyIfPresent(voltmeter, dst, "voltmeterPin");
    copyKeyIfPresent(voltmeter, dst, "voltageDividerR1");
    copyKeyIfPresent(voltmeter, dst, "voltageDividerR2");
    copyKeyIfPresent(voltmeter, dst, "voltmeterCalibration");
    copyKeyIfPresent(voltmeter, dst, "voltmeterOffset");
    copyKeyIfPresent(voltmeter, dst, "slidingWindowSize");
    copyKeyIfPresent(voltmeter, dst, "timerPeriodUs");
    copyKeyIfPresent(voltmeter, dst, "batteryDisconnectThreshold");
    copyKeyIfPresent(voltmeter, dst, "batteryLowThreshold");
  }

  if (src["network"].is<JsonObjectConst>()) {
    const JsonObjectConst network = src["network"].as<JsonObjectConst>();
    copyKeyIfPresent(network, dst, "espNowChannel");
    copyKeyIfPresent(network, dst, "emergencySwitchMacs");
  }

  if (src["estop"].is<JsonObjectConst>()) {
    const JsonObjectConst estop = src["estop"].as<JsonObjectConst>();
    copyMappedKeyIfPresent(estop, dst, "targetMac",           "estopTargetMac");
    copyMappedKeyIfPresent(estop, dst, "switchPin",           "estopSwitchPin");
    copyMappedKeyIfPresent(estop, dst, "switchActiveHigh",    "estopSwitchActiveHigh");
    copyMappedKeyIfPresent(estop, dst, "switchLogicInverted", "estopSwitchLogicInverted");
    copyMappedKeyIfPresent(estop, dst, "routes",              "estopRoutes");

    if (estop["wled"].is<JsonObjectConst>()) {
      const JsonObjectConst estopWled = estop["wled"].as<JsonObjectConst>();
      copyMappedKeyIfPresent(estopWled, dst, "enabled", "estopWledEnabled");
      copyMappedKeyIfPresent(estopWled, dst, "baseUrl", "estopWledBaseUrl");
      copyMappedKeyIfPresent(estopWled, dst, "preset",  "estopWledPreset");
    }
    if (estop["buzzer"].is<JsonObjectConst>()) {
      const JsonObjectConst estopBuzzer = estop["buzzer"].as<JsonObjectConst>();
      copyMappedKeyIfPresent(estopBuzzer, dst, "enabled", "estopBuzzerEnabled");
      copyMappedKeyIfPresent(estopBuzzer, dst, "pin",     "estopBuzzerPin");
    }
  }

  // Safety: ignore any PIN fields if present in structured export.
  dst.remove("pinProtectionEnabled");
  dst.remove("pinCode");
  dst.remove("pinRequired");
  dst.remove("authPin");
}

#if APP_MODE == APP_MODE_ESTOP
static void copyJsonKeyIfPresent(const JsonObjectConst& src, JsonObject& dst, const char* key) {
  if (src[key].isNull()) {
    return;
  }
  dst[key] = src[key];
}

static void buildEStopSettingsResponsePayload(JsonDocument& payload, bool includePinCode) {
  (void)includePinCode;
  const AppSettings& settings = getAppSettings();

  payload["estopTargetMac"]           = settings.estop_target_mac;
  payload["estopSwitchPin"]           = settings.estop_switch_pin;
  payload["estopSwitchActiveHigh"]    = settings.estop_switch_active_high;
  payload["estopSwitchLogicInverted"] = settings.estop_switch_logic_inverted;
  payload["estopWledEnabled"]         = settings.estop_wled_enabled;
  payload["estopWledBaseUrl"]         = settings.estop_wled_base_url;
  payload["estopWledPreset"]          = settings.estop_wled_preset;
  payload["estopBuzzerEnabled"]       = settings.estop_buzzer_enabled;
  payload["estopBuzzerPin"]           = settings.estop_buzzer_pin;

  JsonArray routes = payload["estopRoutes"].to<JsonArray>();
  for (const auto& route : settings.estop_routes) {
    JsonObject routeObj = routes.add<JsonObject>();
    routeObj["targetMac"] = route.target_mac;
    routeObj["switchPin"] = route.switch_pin;
    routeObj["switchActiveHigh"] = route.switch_active_high;
    routeObj["switchLogicInverted"] = route.switch_logic_inverted;
  }
}

static void handleEStopSettingsReadPost() {
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
  buildEStopSettingsResponsePayload(payload, false);
  payload["pinRequired"] = false;

  String output;
  serializeJson(payload, output);
  server.send(200, "application/json", output);
}

static void handleEStopSettingsPost() {
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

  JsonDocument updateDoc;
  JsonObject updateObj = updateDoc.to<JsonObject>();
  copyJsonKeyIfPresent(root, updateObj, "estopTargetMac");
  copyJsonKeyIfPresent(root, updateObj, "estopSwitchPin");
  copyJsonKeyIfPresent(root, updateObj, "estopSwitchActiveHigh");
  copyJsonKeyIfPresent(root, updateObj, "estopSwitchLogicInverted");
  copyJsonKeyIfPresent(root, updateObj, "estopRoutes");
  copyJsonKeyIfPresent(root, updateObj, "estopWledEnabled");
  copyJsonKeyIfPresent(root, updateObj, "estopWledBaseUrl");
  copyJsonKeyIfPresent(root, updateObj, "estopWledPreset");
  copyJsonKeyIfPresent(root, updateObj, "estopBuzzerEnabled");
  copyJsonKeyIfPresent(root, updateObj, "estopBuzzerPin");

  String updateError;
  if (!applySettingsUpdate(updateObj, updateError)) {
    server.send(400, "application/json", buildSettingsResponse(false, updateError));
    return;
  }

  server.send(200, "application/json", buildSettingsResponse(true, "E-STOP settings updated"));
}
#endif

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
  buildStructuredSettingsExportPayload(payload);

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

  String formatError;
  if (!validateStructuredSettingsPayload(updateObj, formatError)) {
    server.send(400, "application/json", buildSettingsResponse(false, formatError));
    return;
  }

  JsonDocument sanitizedDoc;
  JsonObject sanitizedUpdate = sanitizedDoc.to<JsonObject>();
  normalizeStructuredSettingsForImport(updateObj, sanitizedUpdate);
  const JsonObjectConst sanitizedUpdateConst = sanitizedUpdate;

  String updateError;
  if (!applySettingsUpdate(sanitizedUpdateConst, updateError)) {
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
#if APP_MODE == APP_MODE_ESTOP
    if (!sendFileFromSPIFFS("/estop.html")) {
      server.send(404, "text/plain", "estop.html not found");
    }
#else
    if (!sendFileFromSPIFFS("/index.html")) {
      server.send(404, "text/plain", "index.html not found");
    }
#endif
  });

  server.on("/health", HTTP_GET, []() {
    server.send(200, "text/plain", "ok");
  });

  server.on("/device", HTTP_GET, handleDeviceInfoGet);
#if APP_MODE == APP_MODE_ESTOP
  server.on("/estop/status", HTTP_GET, handleEStopStatusGet);
#else
  server.on("/readings", HTTP_GET, []() {
    server.send(200, "application/json", getSensorReadings());
  });

  server.on("/telemetry", HTTP_GET, []() {
    server.send(200, "application/json", telemetryLogGetJson());
  });

  server.on("/power", HTTP_POST, handlePowerPost);
  server.on("/emergency", HTTP_POST, handleEmergencyPost);
#endif

#if APP_MODE == APP_MODE_ESTOP
  server.on("/estop/settings/read", HTTP_POST, handleEStopSettingsReadPost);
  server.on("/estop/settings", HTTP_POST, handleEStopSettingsPost);
  server.on("/estop/settings/read", HTTP_GET, redirectToRoot);
  server.on("/estop/settings", HTTP_GET, redirectToRoot);
#else
  server.on("/power", HTTP_GET, redirectToRoot);
  server.on("/emergency", HTTP_GET, redirectToRoot);
  server.on("/settings/read", HTTP_GET, redirectToRoot);
  server.on("/settings", HTTP_GET, redirectToRoot);
  server.on("/settings/export", HTTP_GET, redirectToRoot);
  server.on("/settings/import", HTTP_GET, redirectToRoot);
  server.on("/settings/unlock", HTTP_GET, redirectToRoot);
  server.on("/settings/reset", HTTP_GET, redirectToRoot);
  server.on("/settings/read", HTTP_POST, handleSettingsReadPost);
  server.on("/settings", HTTP_POST, handleSettingsPost);
  server.on("/settings/export", HTTP_POST, handleSettingsExportPost);
  server.on("/settings/import", HTTP_POST, handleSettingsImportPost);
  server.on("/settings/unlock", HTTP_POST, handleSettingsUnlockPost);
  server.on("/settings/reset", HTTP_POST, handleSettingsResetPost);
#endif
  server.on("/esp/reboot", HTTP_GET, redirectToRoot);
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
      redirectToRoot();
      return;
    }

    if (path == "/") {
#if APP_MODE == APP_MODE_ESTOP
      path = "/estop.html";
#else
      path = "/index.html";
#endif
    }

    // Never expose the other app mode's UI pages even if assets exist in SPIFFS.
    if (isCrossModeUiAssetPath(path)) {
      redirectToRoot();
      return;
    }

    if (sendFileFromSPIFFS(path)) {
      return;
    }
    DAEMON_LOGF("HTTP request not found, redirecting to /: %s\n", path.c_str());
    redirectToRoot();
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
