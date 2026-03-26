#include "web_server_routes.h"

#include "config.h"
#include "app_settings.h"
#include "espnow_comm.h"
#include "led_control.h"
#include "ros_node.h"
#include "wifi_config.h"
#if ENABLE_SENSOR_TASK
#include "voltmeter.h"
#endif

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>

extern WebServer server;

namespace {

bool parseBoolString(const String& value, bool& parsed) {
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

bool parsePowerControlChannel(const String& key, PowerControlChannel& channel) {
  if (key == "controlGroup1") {
    channel = POWER_CHANNEL_GROUP1;
    return true;
  }
  if (key == "controlGroup2") {
    channel = POWER_CHANNEL_GROUP2;
    return true;
  }
  if (key == "controlGroup3") {
    channel = POWER_CHANNEL_GROUP3;
    return true;
  }
  return false;
}

void appendPowerState(JsonDocument& payload) {
  payload["controlGroup1Power"] = getPowerControlState(POWER_CHANNEL_GROUP1);
  payload["controlGroup2Power"] = getPowerControlState(POWER_CHANNEL_GROUP2);
  payload["controlGroup3Power"] = getPowerControlState(POWER_CHANNEL_GROUP3);
}

String buildPowerResponse(bool success, const String& message) {
  JsonDocument payload;
  payload["success"] = success;
  payload["message"] = message;
  appendPowerState(payload);
  String output;
  serializeJson(payload, output);
  return output;
}

String buildSettingsResponse(bool success, const String& message) {
  JsonDocument payload;
  payload["success"] = success;
  payload["message"] = message;
#if APP_MODE == APP_MODE_ESTOP
  payload["pinRequired"] = false;
#else
  AppSettingsReadGuard settingsGuard;
  payload["pinRequired"] = settingsGuard.settings().pin_protection_enabled;
#endif
  String output;
  serializeJson(payload, output);
  return output;
}

bool applyPowerUpdate(const String& controlName, bool enabled) {
  PowerControlChannel channel;
  if (!parsePowerControlChannel(controlName, channel)) {
    return false;
  }
  setPowerControlOverride(channel, enabled);
  return true;
}

bool parseJsonBody(JsonDocument& jsonObj, String& errorMessage) {
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

String jsonTrimmedString(const JsonObjectConst& obj, const char* key) {
  const JsonVariantConst v = obj[key];
  if (v.isNull()) {
    return String();
  }
  String s = v.as<String>();
  s.trim();
  return s;
}

bool authorizeSettingsRequest(const JsonObjectConst& jsonObj) {
#if APP_MODE == APP_MODE_ESTOP
  (void)jsonObj;
  return true;
#else
  return verifySettingsPin(jsonTrimmedString(jsonObj, "authPin"));
#endif
}

bool applySettingsUpdate(const JsonObjectConst& updateObj, String& updateError) {
  if (!updateAppSettingsFromJson(updateObj, updateError)) {
    return false;
  }

  refreshNetworkIdentity();
  refreshESPNowChannel();
  bool runtimeLedEnabled = false;
#if ENABLE_SENSOR_TASK
  bool runtimeSensorEnabled = false;
#endif
  {
    AppSettingsReadGuard settingsGuard;
    runtimeLedEnabled = settingsGuard.settings().runtime_led_enabled;
#if ENABLE_SENSOR_TASK
    runtimeSensorEnabled = settingsGuard.settings().runtime_sensor_enabled;
#endif
  }

  if (runtimeLedEnabled) {
    initLED();
  }
#if ENABLE_SENSOR_TASK
  if (runtimeSensorEnabled) {
    initVoltmeter();
  }
#endif
  return true;
}

void copyKeyIfPresent(const JsonObjectConst& src, JsonObject& dst, const char* key) {
  const JsonVariantConst v = src[key];
  if (v.isNull()) {
    return;
  }
  dst[key] = v;
}

void copyMappedKeyIfPresent(const JsonObjectConst& src, JsonObject& dst, const char* srcKey, const char* dstKey) {
  const JsonVariantConst v = src[srcKey];
  if (v.isNull()) {
    return;
  }
  dst[dstKey] = v;
}

bool looksStructuredSettingsPayload(const JsonObjectConst& src) {
  return src["device"].is<JsonObjectConst>() ||
         src["runtime"].is<JsonObjectConst>() ||
         src["ros"].is<JsonObjectConst>() ||
         src["io"].is<JsonObjectConst>() ||
         src["led"].is<JsonObjectConst>() ||
         src["voltmeter"].is<JsonObjectConst>() ||
         src["network"].is<JsonObjectConst>() ||
         src["estop"].is<JsonObjectConst>();
}

bool validateStructuredSettingsPayload(const JsonObjectConst& src, String& error) {
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
  if (schemaVersion != 3) {
    error = "Unsupported schemaVersion (expected 3)";
    return false;
  }

  if (!looksStructuredSettingsPayload(src)) {
    error = "Unsupported import payload: missing structured sections";
    return false;
  }

  return true;
}

void buildStructuredSettingsExportPayload(JsonDocument& payload) {
  JsonDocument flat;
  appSettingsToJson(flat, false);
  flat.remove("pinProtectionEnabled");
  flat.remove("pinCode");
  flat.remove("pinRequired");

  const JsonObjectConst src = flat.as<JsonObjectConst>();

  payload["schema"]         = "esp-daemon.settings-export";
  payload["schemaVersion"]  = 3;
  payload["generatedBy"]    = "esp-daemon";
  payload["fwVersion"]      = ESP_DAEMON_FW_VERSION;

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
  copyMappedKeyIfPresent(src, ioSwitches, "controlGroup1SwitchPin", "controlGroup1Pin");
  copyMappedKeyIfPresent(src, ioSwitches, "controlGroup2SwitchPin", "controlGroup2Pin");
  copyMappedKeyIfPresent(src, ioSwitches, "controlGroup3SwitchPin", "controlGroup3Pin");

  JsonObject ioPower = io["powerOutputs"].to<JsonObject>();
  copyMappedKeyIfPresent(src, ioPower, "controlGroup1PowerPin",    "controlGroup1Pin");
  copyMappedKeyIfPresent(src, ioPower, "controlGroup2Power12vPin", "controlGroup2Power12vPin");
  copyMappedKeyIfPresent(src, ioPower, "controlGroup2Power7v4Pin", "controlGroup2Power7v4Pin");
  copyMappedKeyIfPresent(src, ioPower, "controlGroup3PowerPin",    "controlGroup3Pin");

  JsonObject ioGroups = io["groups"].to<JsonObject>();
  copyMappedKeyIfPresent(src, ioGroups, "controlGroup1Name", "controlGroup1");
  copyMappedKeyIfPresent(src, ioGroups, "controlGroup2Name", "controlGroup2");
  copyMappedKeyIfPresent(src, ioGroups, "controlGroup3Name", "controlGroup3");

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
  copyKeyIfPresent(src, network, "emergencySources");

  JsonObject estop = payload["estop"].to<JsonObject>();
  copyMappedKeyIfPresent(src, estop, "estopTargetMac",           "targetMac");
  copyMappedKeyIfPresent(src, estop, "estopSwitchPin",           "switchPin");
  copyMappedKeyIfPresent(src, estop, "estopSwitchActiveHigh",    "switchActiveHigh");
  copyMappedKeyIfPresent(src, estop, "estopSwitchLogicInverted", "switchLogicInverted");
  copyMappedKeyIfPresent(src, estop, "estopRoutes",              "routes");

  JsonObject estopWled = estop["wled"].to<JsonObject>();
  copyMappedKeyIfPresent(src, estopWled, "estopWledEnabled", "enabled");
  copyMappedKeyIfPresent(src, estopWled, "estopWledBaseUrl", "baseUrl");
  copyMappedKeyIfPresent(src, estopWled, "estopWledPressedPreset",  "pressedPreset");
  copyMappedKeyIfPresent(src, estopWled, "estopWledReleasedPreset", "releasedPreset");

  JsonObject estopBuzzer = estop["buzzer"].to<JsonObject>();
  copyMappedKeyIfPresent(src, estopBuzzer, "estopBuzzerEnabled", "enabled");
  copyMappedKeyIfPresent(src, estopBuzzer, "estopBuzzerPin",     "pin");
}

void normalizeStructuredSettingsForImport(const JsonObjectConst& src, JsonObject& dst) {
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
      copyMappedKeyIfPresent(ioSwitches, dst, "controlGroup1Pin", "controlGroup1SwitchPin");
      copyMappedKeyIfPresent(ioSwitches, dst, "controlGroup2Pin", "controlGroup2SwitchPin");
      copyMappedKeyIfPresent(ioSwitches, dst, "controlGroup3Pin", "controlGroup3SwitchPin");
    }
    if (io["powerOutputs"].is<JsonObjectConst>()) {
      const JsonObjectConst ioPower = io["powerOutputs"].as<JsonObjectConst>();
      copyMappedKeyIfPresent(ioPower, dst, "controlGroup1Pin",         "controlGroup1PowerPin");
      copyMappedKeyIfPresent(ioPower, dst, "controlGroup2Power12vPin", "controlGroup2Power12vPin");
      copyMappedKeyIfPresent(ioPower, dst, "controlGroup2Power7v4Pin", "controlGroup2Power7v4Pin");
      copyMappedKeyIfPresent(ioPower, dst, "controlGroup3Pin",         "controlGroup3PowerPin");
    }
    if (io["groups"].is<JsonObjectConst>()) {
      const JsonObjectConst ioGroups = io["groups"].as<JsonObjectConst>();
      copyMappedKeyIfPresent(ioGroups, dst, "controlGroup1", "controlGroup1Name");
      copyMappedKeyIfPresent(ioGroups, dst, "controlGroup2", "controlGroup2Name");
      copyMappedKeyIfPresent(ioGroups, dst, "controlGroup3", "controlGroup3Name");
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
    copyKeyIfPresent(network, dst, "emergencySources");
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
      copyMappedKeyIfPresent(estopWled, dst, "pressedPreset",  "estopWledPressedPreset");
      copyMappedKeyIfPresent(estopWled, dst, "releasedPreset", "estopWledReleasedPreset");
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

} // namespace
#if APP_MODE == APP_MODE_ESTOP
static void copyJsonKeyIfPresent(const JsonObjectConst& src, JsonObject& dst, const char* key) {
  if (src[key].isNull()) {
    return;
  }
  dst[key] = src[key];
}

static void buildEStopSettingsResponsePayload(JsonDocument& payload, bool includePinCode) {
  (void)includePinCode;
  AppSettingsReadGuard settingsGuard;
  const AppSettings& settings = settingsGuard.settings();

  payload["estopTargetMac"]           = settings.estop_target_mac;
  payload["estopSwitchPin"]           = settings.estop_switch_pin;
  payload["estopSwitchActiveHigh"]    = settings.estop_switch_active_high;
  payload["estopSwitchLogicInverted"] = settings.estop_switch_logic_inverted;
  payload["estopWledEnabled"]         = settings.estop_wled_enabled;
  payload["estopWledBaseUrl"]         = settings.estop_wled_base_url;
  payload["estopWledPressedPreset"]   = settings.estop_wled_pressed_preset;
  payload["estopWledReleasedPreset"]  = settings.estop_wled_released_preset;
  payload["estopBuzzerEnabled"]       = settings.estop_buzzer_enabled;
  payload["estopBuzzerPin"]           = settings.estop_buzzer_pin;

  JsonArray routes = payload["estopRoutes"].to<JsonArray>();
  for (const auto& route : settings.estop_routes) {
    JsonObject routeObj = routes.add<JsonObject>();
    routeObj["targetMac"]             = route.target_mac;
    routeObj["switchPin"]             = route.switch_pin;
    routeObj["switchActiveHigh"]      = route.switch_active_high;
    routeObj["switchLogicInverted"]   = route.switch_logic_inverted;
  }
}

static bool validateEStopStructuredSettingsPayload(const JsonObjectConst& src, String& error) {
  if (!src["schema"].is<const char*>()) {
    error = "Unsupported import format: missing schema";
    return false;
  }
  const String schema = String(src["schema"].as<const char*>());
  if (schema != "esp-estop.settings-export") {
    error = "Unsupported import schema";
    return false;
  }

  if (!src["schemaVersion"].is<int>()) {
    error = "Unsupported import format: missing schemaVersion";
    return false;
  }
  const int schemaVersion = src["schemaVersion"].as<int>();
  if (schemaVersion != 4) {
    error = "Unsupported schemaVersion (expected 4)";
    return false;
  }

  if (!src["estop"].is<JsonObjectConst>()) {
    error = "Unsupported import payload: missing estop section";
    return false;
  }

  return true;
}

static void buildEStopStructuredSettingsExportPayload(JsonDocument& payload) {
  AppSettingsReadGuard settingsGuard;
  const AppSettings& settings = settingsGuard.settings();

  payload["schema"]             = "esp-estop.settings-export";
  payload["schemaVersion"]      = 4;
  payload["generatedBy"]        = "esp-estop";
  payload["fwVersion"]          = ESP_DAEMON_FW_VERSION;

  JsonObject estop = payload["estop"].to<JsonObject>();
  estop["targetMac"]            = settings.estop_target_mac;
  estop["switchPin"]            = settings.estop_switch_pin;
  estop["switchActiveHigh"]     = settings.estop_switch_active_high;
  estop["switchLogicInverted"]  = settings.estop_switch_logic_inverted;

  JsonArray routes = estop["routes"].to<JsonArray>();
  for (const auto& route : settings.estop_routes) {
    JsonObject routeObj = routes.add<JsonObject>();
    routeObj["targetMac"]           = route.target_mac;
    routeObj["switchPin"]           = route.switch_pin;
    routeObj["switchActiveHigh"]    = route.switch_active_high;
    routeObj["switchLogicInverted"] = route.switch_logic_inverted;
  }

  JsonObject wled = estop["wled"].to<JsonObject>();
  wled["enabled"]         = settings.estop_wled_enabled;
  wled["baseUrl"]         = settings.estop_wled_base_url;
  wled["pressedPreset"]   = settings.estop_wled_pressed_preset;
  wled["releasedPreset"]  = settings.estop_wled_released_preset;

  JsonObject buzzer = estop["buzzer"].to<JsonObject>();
  buzzer["enabled"]       = settings.estop_buzzer_enabled;
  buzzer["pin"]           = settings.estop_buzzer_pin;
}

static void normalizeEStopStructuredSettingsForImport(const JsonObjectConst& src, JsonObject& dst) {
  if (!src["estop"].is<JsonObjectConst>()) {
    return;
  }

  const JsonObjectConst estop = src["estop"].as<JsonObjectConst>();
  copyMappedKeyIfPresent(estop, dst, "targetMac",           "estopTargetMac");
  copyMappedKeyIfPresent(estop, dst, "switchPin",           "estopSwitchPin");
  copyMappedKeyIfPresent(estop, dst, "switchActiveHigh",    "estopSwitchActiveHigh");
  copyMappedKeyIfPresent(estop, dst, "switchLogicInverted", "estopSwitchLogicInverted");
  copyMappedKeyIfPresent(estop, dst, "routes",              "estopRoutes");

  if (estop["wled"].is<JsonObjectConst>()) {
    const JsonObjectConst wled = estop["wled"].as<JsonObjectConst>();
    copyMappedKeyIfPresent(wled, dst, "enabled",            "estopWledEnabled");
    copyMappedKeyIfPresent(wled, dst, "baseUrl",            "estopWledBaseUrl");
    copyMappedKeyIfPresent(wled, dst, "pressedPreset",      "estopWledPressedPreset");
    copyMappedKeyIfPresent(wled, dst, "releasedPreset",     "estopWledReleasedPreset");
  }

  if (estop["buzzer"].is<JsonObjectConst>()) {
    const JsonObjectConst buzzer = estop["buzzer"].as<JsonObjectConst>();
    copyMappedKeyIfPresent(buzzer, dst, "enabled",          "estopBuzzerEnabled");
    copyMappedKeyIfPresent(buzzer, dst, "pin",              "estopBuzzerPin");
  }

  dst.remove("pinProtectionEnabled");
  dst.remove("pinCode");
  dst.remove("pinRequired");
  dst.remove("authPin");
}

void handleEStopSettingsReadPost() {
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

void handleEStopSettingsPost() {
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
  copyJsonKeyIfPresent(root, updateObj, "estopWledPressedPreset");
  copyJsonKeyIfPresent(root, updateObj, "estopWledReleasedPreset");
  copyJsonKeyIfPresent(root, updateObj, "estopBuzzerEnabled");
  copyJsonKeyIfPresent(root, updateObj, "estopBuzzerPin");

  String updateError;
  if (!applySettingsUpdate(updateObj, updateError)) {
    server.send(400, "application/json", buildSettingsResponse(false, updateError));
    return;
  }

  server.send(200, "application/json", buildSettingsResponse(true, "E-STOP settings updated"));
}

void handleEStopSettingsExportPost() {
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
  buildEStopStructuredSettingsExportPayload(payload);

  String output;
  serializeJsonPretty(payload, output);
  server.sendHeader("Content-Disposition", "attachment; filename=\"estop_settings_export.json\"");
  server.send(200, "application/json", output);
}

void handleEStopSettingsImportPost() {
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
  if (!validateEStopStructuredSettingsPayload(updateObj, formatError)) {
    server.send(400, "application/json", buildSettingsResponse(false, formatError));
    return;
  }

  JsonDocument normalizedDoc;
  JsonObject normalizedUpdate = normalizedDoc.to<JsonObject>();
  normalizeEStopStructuredSettingsForImport(updateObj, normalizedUpdate);

  String updateError;
  if (!applySettingsUpdate(normalizedUpdate, updateError)) {
    server.send(400, "application/json", buildSettingsResponse(false, updateError));
    return;
  }

  server.send(200, "application/json", buildSettingsResponse(true, "E-STOP settings imported"));
}

void handleEStopSettingsRestoreDefaultsPost() {
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
  bool runtimeLedEnabled = false;
#if ENABLE_SENSOR_TASK
  bool runtimeSensorEnabled = false;
#endif
  {
    AppSettingsReadGuard settingsGuard;
    runtimeLedEnabled = settingsGuard.settings().runtime_led_enabled;
#if ENABLE_SENSOR_TASK
    runtimeSensorEnabled = settingsGuard.settings().runtime_sensor_enabled;
#endif
  }
  if (runtimeLedEnabled) {
    initLED();
  }
#if ENABLE_SENSOR_TASK
  if (runtimeSensorEnabled) {
    initVoltmeter();
  }
#endif

  server.send(200, "application/json", buildSettingsResponse(true, "E-STOP settings restored to defaults"));
}

void handleEStopSettingsFactoryResetPost() {
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

  if (!eraseAppSettingsFromNvs()) {
    server.send(400, "application/json", buildSettingsResponse(false, "Failed to erase NVS settings"));
    return;
  }

  refreshNetworkIdentity();
  refreshESPNowChannel();
  bool runtimeLedEnabled = false;
#if ENABLE_SENSOR_TASK
  bool runtimeSensorEnabled = false;
#endif
  {
    AppSettingsReadGuard settingsGuard;
    runtimeLedEnabled = settingsGuard.settings().runtime_led_enabled;
#if ENABLE_SENSOR_TASK
    runtimeSensorEnabled = settingsGuard.settings().runtime_sensor_enabled;
#endif
  }
  if (runtimeLedEnabled) {
    initLED();
  }
#if ENABLE_SENSOR_TASK
  if (runtimeSensorEnabled) {
    initVoltmeter();
  }
#endif

  server.send(200, "application/json", buildSettingsResponse(true, "Factory reset complete (full NVS erased), rebooting..."));
  delay(50);
  ESP.restart();
}
#endif

void handlePowerPost() {
  bool updated = false;

  if (server.hasArg("control") && server.hasArg("enabled")) {
    bool enabled = false;
    if (parseBoolString(server.arg("enabled"), enabled) &&
        applyPowerUpdate(server.arg("control"), enabled)) {
      updated = true;
    }
  }

  if (server.hasArg("controlGroup1Power")) {
    bool enabled = false;
    if (parseBoolString(server.arg("controlGroup1Power"), enabled)) {
      setPowerControlOverride(POWER_CHANNEL_GROUP1, enabled);
      updated = true;
    }
  }
  if (server.hasArg("controlGroup2Power")) {
    bool enabled = false;
    if (parseBoolString(server.arg("controlGroup2Power"), enabled)) {
      setPowerControlOverride(POWER_CHANNEL_GROUP2, enabled);
      updated = true;
    }
  }
  if (server.hasArg("controlGroup3Power")) {
    bool enabled = false;
    if (parseBoolString(server.arg("controlGroup3Power"), enabled)) {
      setPowerControlOverride(POWER_CHANNEL_GROUP3, enabled);
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
      if (jsonObj["controlGroup1Power"].is<bool>()) {
        setPowerControlOverride(POWER_CHANNEL_GROUP1, jsonObj["controlGroup1Power"].as<bool>());
        updated = true;
      }
      if (jsonObj["controlGroup2Power"].is<bool>()) {
        setPowerControlOverride(POWER_CHANNEL_GROUP2, jsonObj["controlGroup2Power"].as<bool>());
        updated = true;
      }
      if (jsonObj["controlGroup3Power"].is<bool>()) {
        setPowerControlOverride(POWER_CHANNEL_GROUP3, jsonObj["controlGroup3Power"].as<bool>());
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

void handleEmergencyPost() {
  bool updated = false;
  bool emergency = false;

  if (server.hasArg("emergency") && parseBoolString(server.arg("emergency"), emergency)) {
    setPowerControlOverride(POWER_CHANNEL_GROUP1, emergency);
    updated = true;
  }

  if (server.hasArg("plain")) {
    JsonDocument jsonObj;
    DeserializationError error = deserializeJson(jsonObj, server.arg("plain"));
    if (!error && jsonObj["emergency"].is<bool>()) {
      setPowerControlOverride(POWER_CHANNEL_GROUP1, jsonObj["emergency"].as<bool>());
      updated = true;
    }
  }

  if (updated) {
    server.send(200, "application/json", buildPowerResponse(true, "Control Group 1 power updated"));
  } else {
    server.send(400, "application/json", buildPowerResponse(false, "Invalid emergency payload"));
  }
}

void handleSettingsReadPost() {
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
  {
    AppSettingsReadGuard settingsGuard;
    payload["pinRequired"] = settingsGuard.settings().pin_protection_enabled;
  }

  String output;
  serializeJson(payload, output);
  server.send(200, "application/json", output);
}

void handleSettingsPost() {
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

void handleSettingsExportPost() {
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

void handleSettingsImportPost() {
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

void handleSettingsUnlockPost() {
  JsonDocument jsonObj;
  String parseError;
  if (!parseJsonBody(jsonObj, parseError)) {
    server.send(400, "application/json", buildSettingsResponse(false, parseError));
    return;
  }

  {
    AppSettingsReadGuard settingsGuard;
    if (!settingsGuard.settings().pin_protection_enabled) {
      server.send(200, "application/json", buildSettingsResponse(true, "OK"));
      return;
    }
  }

  const String pin = jsonTrimmedString(jsonObj.as<JsonObjectConst>(), "pin");
  if (verifySettingsPin(pin)) {
    server.send(200, "application/json", buildSettingsResponse(true, "OK"));
    return;
  }

  server.send(403, "application/json", buildSettingsResponse(false, "Invalid PIN"));
}

void handleSettingsRestoreDefaultsPost() {
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
  bool runtimeLedEnabled = false;
#if ENABLE_SENSOR_TASK
  bool runtimeSensorEnabled = false;
#endif
  {
    AppSettingsReadGuard settingsGuard;
    runtimeLedEnabled = settingsGuard.settings().runtime_led_enabled;
#if ENABLE_SENSOR_TASK
    runtimeSensorEnabled = settingsGuard.settings().runtime_sensor_enabled;
#endif
  }
  if (runtimeLedEnabled) {
    initLED();
  }
#if ENABLE_SENSOR_TASK
  if (runtimeSensorEnabled) {
    initVoltmeter();
  }
#endif

  server.send(200, "application/json", buildSettingsResponse(true, "Settings restored to defaults"));
}

void handleSettingsFactoryResetPost() {
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

  if (!eraseAppSettingsFromNvs()) {
    server.send(400, "application/json", buildSettingsResponse(false, "Failed to erase NVS settings"));
    return;
  }

  refreshNetworkIdentity();
  refreshESPNowChannel();
  bool runtimeLedEnabled = false;
#if ENABLE_SENSOR_TASK
  bool runtimeSensorEnabled = false;
#endif
  {
    AppSettingsReadGuard settingsGuard;
    runtimeLedEnabled = settingsGuard.settings().runtime_led_enabled;
#if ENABLE_SENSOR_TASK
    runtimeSensorEnabled = settingsGuard.settings().runtime_sensor_enabled;
#endif
  }
  if (runtimeLedEnabled) {
    initLED();
  }
#if ENABLE_SENSOR_TASK
  if (runtimeSensorEnabled) {
    initVoltmeter();
  }
#endif

  server.send(200, "application/json", buildSettingsResponse(true, "Factory reset complete (full NVS erased), rebooting..."));
  delay(50);
  ESP.restart();
}

void handleEspRebootPost() {
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
