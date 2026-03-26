#include "app_settings.h"
#include "app_settings_internal.h"
#include "config.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <freertos/semphr.h>
#include <cstring>

namespace {

          const char* kSettingsNvsNamespace     = "espd_cfg";
          const char* kSettingsNvsKey           = "settings_json";
constexpr size_t      kMaxEmergencySources      = 16;
constexpr size_t      kMaxEStopRoutes           = 16;
constexpr uint8_t     kEspNowChannelDefault     = 6;
constexpr const char* kDefaultControlGroupName1 = "Chassis Power";
constexpr const char* kDefaultControlGroupName2 = "Actuators Power";
constexpr const char* kDefaultControlGroupName3 = "Others Power";

/** Default status-page Control Panel shortcut when unset or empty in JSON. */
constexpr const char* kDefaultControlPanelUrl = "https://scx.tw/links";

AppSettings g_settings;
SemaphoreHandle_t g_settings_mutex = nullptr;

void ensureSettingsMutex() {
  if (g_settings_mutex != nullptr) {
    return;
  }
  g_settings_mutex = xSemaphoreCreateRecursiveMutex();
}

void lockSettings() {
  ensureSettingsMutex();
  if (g_settings_mutex != nullptr) {
    xSemaphoreTakeRecursive(g_settings_mutex, portMAX_DELAY);
  }
}

void unlockSettings() {
  if (g_settings_mutex != nullptr) {
    xSemaphoreGiveRecursive(g_settings_mutex);
  }
}

class AppSettingsWriteGuard {
public:
  AppSettingsWriteGuard() {
    lockSettings();
  }

  ~AppSettingsWriteGuard() {
    unlockSettings();
  }

  AppSettingsWriteGuard(const AppSettingsWriteGuard&) = delete;
  AppSettingsWriteGuard& operator=(const AppSettingsWriteGuard&) = delete;
};

EStopRouteConfig buildDefaultEStopRoute() {
  EStopRouteConfig route = {};
  route.target_mac            = "";
  route.switch_pin            = 4;
  route.switch_active_high    = false;
  route.switch_logic_inverted = false;
  return route;
}

void normalizeEStopRoute(EStopRouteConfig& route) {
  route.target_mac.replace("-", ":");
  route.target_mac.trim();
  route.target_mac.toLowerCase();
  if (route.target_mac.length() == 0) {
    return;
  }

  std::array<uint8_t, 6> parsedMac = {};
  if (parseMacString(route.target_mac, parsedMac)) {
    route.target_mac = toMacString(parsedMac);
  }
}

void syncLegacyEStopFieldsFromRoutes() {
  if (g_settings.estop_routes.empty()) {
    const EStopRouteConfig route = buildDefaultEStopRoute();
    g_settings.estop_target_mac            = route.target_mac;
    g_settings.estop_switch_pin            = route.switch_pin;
    g_settings.estop_switch_active_high    = route.switch_active_high;
    g_settings.estop_switch_logic_inverted = route.switch_logic_inverted;
    return;
  }

  const EStopRouteConfig& primary = g_settings.estop_routes.front();
  g_settings.estop_target_mac            = primary.target_mac;
  g_settings.estop_switch_pin            = primary.switch_pin;
  g_settings.estop_switch_active_high    = primary.switch_active_high;
  g_settings.estop_switch_logic_inverted = primary.switch_logic_inverted;
}

void syncRoutesFromLegacyEStopFields() {
  g_settings.estop_routes.clear();

  EStopRouteConfig route = {};
  route.target_mac            = g_settings.estop_target_mac;
  route.switch_pin            = g_settings.estop_switch_pin;
  route.switch_active_high    = g_settings.estop_switch_active_high;
  route.switch_logic_inverted = g_settings.estop_switch_logic_inverted;
  normalizeEStopRoute(route);
  g_settings.estop_routes.push_back(route);

  syncLegacyEStopFieldsFromRoutes();
}

String toLowerCopy(String value) {
  value.toLowerCase();
  return value;
}

String sanitizeDeviceName(String value) {
  value = toLowerCopy(value);
  value.trim();
  if (value.length() == 0) {
    return value;
  }

  String out;
  out.reserve(value.length());
  for (size_t i = 0; i < value.length(); ++i) {
    const char ch = value[i];
    const bool valid = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    if (valid) {
      out += ch;
    }
  }
  return out;
}

void normalizeControlGroupName(String& value, const char* fallback) {
  value.trim();
  if (value.length() == 0) {
    value = fallback;
  }
}

String defaultDeviceNameFromMac() {
  const uint64_t efuse = ESP.getEfuseMac();
  const uint32_t macTail = static_cast<uint32_t>(efuse & 0xFFFFFFULL);
  char tail[7];
  snprintf(tail, sizeof(tail), "%06x", static_cast<unsigned>(macTail));
#if APP_MODE == APP_MODE_ESTOP
  return String("esp-estop_") + String(tail);
#else
  return String("esp-daemon_") + String(tail);
#endif
}

void updateCalibrationFromDivider() {
  if (g_settings.voltage_divider_r2 <= 0.0f) {
    return;
  }
  g_settings.voltmeter_calibration =
    (g_settings.voltage_divider_r1 + g_settings.voltage_divider_r2) / g_settings.voltage_divider_r2;
}

void applyDefaults() {
  g_settings.device_name                  = defaultDeviceNameFromMac();

  g_settings.pin_protection_enabled       = false;
  g_settings.pin_code                     = "1234";

#if APP_MODE == APP_MODE_ESTOP
  g_settings.runtime_espnow_enabled       = true;
  g_settings.runtime_microros_enabled     = false;
  g_settings.runtime_led_enabled          = false;
  g_settings.runtime_sensor_enabled       = false;
#else
  g_settings.runtime_espnow_enabled       = true;
  g_settings.runtime_microros_enabled     = true;
  g_settings.runtime_led_enabled          = true;
  g_settings.runtime_sensor_enabled       = true;
#endif

  g_settings.control_group1_name          = kDefaultControlGroupName1;
  g_settings.control_group2_name          = kDefaultControlGroupName2;
  g_settings.control_group3_name          = kDefaultControlGroupName3;

  g_settings.control_group1_switch_pin    = 8;
  g_settings.control_group2_switch_pin    = 9;
  g_settings.control_group3_switch_pin    = 20;

  g_settings.control_group1_power_pin     = 4;
  g_settings.control_group2_power_12v_pin = 3;
  g_settings.control_group2_power_7v4_pin = 5;
  g_settings.control_group3_power_pin     = 10;

  g_settings.switch_active_high           = false;
  g_settings.power_active_high            = false;

  g_settings.led_pin                      = 6;
  g_settings.led_count                    = 40;
  g_settings.led_brightness               = 200;
  g_settings.led_override_duration_ms     = 1000;

  g_settings.voltmeter_pin                = 2;
  g_settings.voltage_divider_r1           = 47000.0f;
  g_settings.voltage_divider_r2           = 4700.0f;
  g_settings.voltmeter_calibration        = 11.0f;
  g_settings.voltmeter_offset             = 0.0f;
  g_settings.sliding_window_size          = 64;
  g_settings.timer_period_us              = 1000000;
  g_settings.battery_disconnect_threshold = 3.0f;
  g_settings.battery_low_threshold        = 17.5f;
  updateCalibrationFromDivider();

  g_settings.ros_node_name                = "esp_daemon";
  g_settings.ros_domain_id                = 0;
  g_settings.ros_timer_ms                 = 100;
  g_settings.mros_timeout_ms              = 100;
  g_settings.mros_ping_interval_ms        = 1000;
  g_settings.espnow_channel               = kEspNowChannelDefault;
  g_settings.estop_target_mac             = "";
  g_settings.estop_switch_pin             = 4;
  g_settings.estop_switch_active_high     = false;
  g_settings.estop_switch_logic_inverted  = false;
  g_settings.estop_wled_enabled           = false;
  g_settings.estop_wled_base_url          = "";
  g_settings.estop_wled_pressed_preset    = 1;
  g_settings.estop_wled_released_preset   = 1;
  g_settings.estop_buzzer_enabled         = true;
  g_settings.estop_buzzer_pin             = 5;
  syncRoutesFromLegacyEStopFields();

  g_settings.emergency_sources.clear();

  g_settings.control_panel_url = kDefaultControlPanelUrl;
}

void loadBool(const JsonObjectConst& json, const char* key, bool& value) {
  const JsonVariantConst v = json[key];

  if (v.is<bool>()) {
    value = v.as<bool>();
    return;
  }

  if (v.is<int>()) {
    value = v.as<int>() != 0;
    return;
  }
  if (v.is<unsigned long>()) {
    value = v.as<unsigned long>() != 0;
    return;
  }

  if (v.is<const char*>()) {
    String s = String(v.as<const char*>());
    s.trim();
    value = (s.equalsIgnoreCase("true") || s == "1");
    return;
  }
  if (v.is<String>()) {
    String s = v.as<String>();
    s.trim();
    value = (s.equalsIgnoreCase("true") || s == "1");
    return;
  }
}

void loadUInt8(const JsonObjectConst& json, const char* key, uint8_t& value) {
  if (json[key].is<int>()) {
    int v = json[key].as<int>();
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    value = static_cast<uint8_t>(v);
  }
}

void loadUInt16(const JsonObjectConst& json, const char* key, uint16_t& value) {
  if (json[key].is<int>()) {
    int v = json[key].as<int>();
    if (v < 1) v = 1;
    if (v > 65535) v = 65535;
    value = static_cast<uint16_t>(v);
  }
}

void loadUInt32(const JsonObjectConst& json, const char* key, uint32_t& value) {
  if (json[key].is<unsigned long>()) {
    value = json[key].as<unsigned long>();
  } else if (json[key].is<int>()) {
    int v = json[key].as<int>();
    if (v < 0) v = 0;
    value = static_cast<uint32_t>(v);
  }
}

void loadFloat(const JsonObjectConst& json, const char* key, float& value) {
  if (json[key].is<float>() || json[key].is<double>() || json[key].is<int>()) {
    value = json[key].as<float>();
  }
}

void loadString(const JsonObjectConst& json, const char* key, String& value) {
  const JsonVariantConst v = json[key];
  if (v.isNull()) {
    return;
  }
  value = v.as<String>();
  value.trim();
}

bool validateControlPanelUrl(const String& url) {
  if (url.length() > 192) {
    return false;
  }
  if (url.length() == 0) {
    return true;
  }
  if (url.startsWith("http://") || url.startsWith("https://")) {
    return true;
  }
  if (url.startsWith("/")) {
    if (url.startsWith("//")) {
      return false;
    }
    return true;
  }
  return false;
}

void loadEStopRoutes(const JsonObjectConst& json) {
  bool hasRouteArray = false;
  if (json["estopRoutes"].is<JsonArrayConst>()) {
    hasRouteArray = true;
    g_settings.estop_routes.clear();
    for (JsonVariantConst entry : json["estopRoutes"].as<JsonArrayConst>()) {
      if (!entry.is<JsonObjectConst>()) {
        continue;
      }
      JsonObjectConst routeObj = entry.as<JsonObjectConst>();
      EStopRouteConfig route = buildDefaultEStopRoute();
      loadString(routeObj, "targetMac", route.target_mac);
      loadUInt8(routeObj, "switchPin", route.switch_pin);
      loadBool(routeObj, "switchActiveHigh", route.switch_active_high);
      loadBool(routeObj, "switchLogicInverted", route.switch_logic_inverted);
      normalizeEStopRoute(route);
      g_settings.estop_routes.push_back(route);
      if (g_settings.estop_routes.size() >= kMaxEStopRoutes) {
        break;
      }
    }
  }

  if (!hasRouteArray || g_settings.estop_routes.empty()) {
    syncRoutesFromLegacyEStopFields();
  } else {
    syncLegacyEStopFieldsFromRoutes();
  }
}

void loadFromJson(const JsonObjectConst& json) {
  loadString(json, "deviceName", g_settings.device_name);
  g_settings.device_name = sanitizeDeviceName(g_settings.device_name);
  if (g_settings.device_name.length() == 0) {
    g_settings.device_name = defaultDeviceNameFromMac();
  }

  loadBool(json,   "pinProtectionEnabled",          g_settings.pin_protection_enabled);
  loadString(json, "pinCode",                       g_settings.pin_code);

  loadBool(json,   "runtimeEspNowEnabled",          g_settings.runtime_espnow_enabled);
  loadBool(json,   "runtimeMicroRosEnabled",        g_settings.runtime_microros_enabled);
  loadBool(json,   "runtimeLedEnabled",             g_settings.runtime_led_enabled);
  loadBool(json,   "runtimeSensorEnabled",          g_settings.runtime_sensor_enabled);
  loadString(json, "controlGroup1Name",             g_settings.control_group1_name);
  loadString(json, "controlGroup2Name",             g_settings.control_group2_name);
  loadString(json, "controlGroup3Name",             g_settings.control_group3_name);
  normalizeControlGroupName(g_settings.control_group1_name, kDefaultControlGroupName1);
  normalizeControlGroupName(g_settings.control_group2_name, kDefaultControlGroupName2);
  normalizeControlGroupName(g_settings.control_group3_name, kDefaultControlGroupName3);

  loadUInt8(json,  "controlGroup1SwitchPin",        g_settings.control_group1_switch_pin);
  loadUInt8(json,  "controlGroup2SwitchPin",        g_settings.control_group2_switch_pin);
  loadUInt8(json,  "controlGroup3SwitchPin",        g_settings.control_group3_switch_pin);

  loadUInt8(json,  "controlGroup1PowerPin",         g_settings.control_group1_power_pin);
  loadUInt8(json,  "controlGroup2Power12vPin",      g_settings.control_group2_power_12v_pin);
  loadUInt8(json,  "controlGroup2Power7v4Pin",      g_settings.control_group2_power_7v4_pin);
  loadUInt8(json,  "controlGroup3PowerPin",         g_settings.control_group3_power_pin);

  loadBool(json,   "switchActiveHigh",              g_settings.switch_active_high);
  loadBool(json,   "powerActiveHigh",               g_settings.power_active_high);

  loadUInt8(json,  "ledPin",                        g_settings.led_pin);
  loadUInt16(json, "ledCount",                      g_settings.led_count);
  loadUInt8(json,  "ledBrightness",                 g_settings.led_brightness);
  loadUInt32(json, "ledOverrideDurationMs",         g_settings.led_override_duration_ms);

  loadUInt8(json,  "voltmeterPin",                  g_settings.voltmeter_pin);
  loadFloat(json,  "voltageDividerR1",              g_settings.voltage_divider_r1);
  loadFloat(json,  "voltageDividerR2",              g_settings.voltage_divider_r2);
  loadFloat(json,  "voltmeterCalibration",          g_settings.voltmeter_calibration);
  loadFloat(json,  "voltmeterOffset",               g_settings.voltmeter_offset);
  loadUInt16(json, "slidingWindowSize",             g_settings.sliding_window_size);
  loadUInt32(json, "timerPeriodUs",                 g_settings.timer_period_us);
  loadFloat(json,  "batteryDisconnectThreshold",    g_settings.battery_disconnect_threshold);
  loadFloat(json,  "batteryLowThreshold",           g_settings.battery_low_threshold);
  updateCalibrationFromDivider();

  loadString(json, "rosNodeName", g_settings.ros_node_name);
  if (g_settings.ros_node_name.length() == 0) {
    g_settings.ros_node_name = "esp_daemon";
  }
  if (json["rosDomainId"].is<int>()) {
    g_settings.ros_domain_id = json["rosDomainId"].as<int>();
  }
  loadUInt32(json, "rosTimerMs",                    g_settings.ros_timer_ms);
  loadUInt32(json, "mrosTimeoutMs",                 g_settings.mros_timeout_ms);
  loadUInt32(json, "mrosPingIntervalMs",            g_settings.mros_ping_interval_ms);
  loadUInt8(json,  "espNowChannel",                 g_settings.espnow_channel);
  loadString(json, "estopTargetMac",                g_settings.estop_target_mac);
  loadUInt8(json,  "estopSwitchPin",                g_settings.estop_switch_pin);
  loadBool(json,   "estopSwitchActiveHigh",         g_settings.estop_switch_active_high);
  loadBool(json,   "estopSwitchLogicInverted",      g_settings.estop_switch_logic_inverted);
  loadBool(json,   "estopWledEnabled",              g_settings.estop_wled_enabled);
  loadString(json, "estopWledBaseUrl",              g_settings.estop_wled_base_url);
  loadUInt16(json, "estopWledPressedPreset",        g_settings.estop_wled_pressed_preset);
  loadUInt16(json, "estopWledReleasedPreset",       g_settings.estop_wled_released_preset);
  loadBool(json,   "estopBuzzerEnabled",            g_settings.estop_buzzer_enabled);
  loadUInt8(json,  "estopBuzzerPin",                g_settings.estop_buzzer_pin);
  syncRoutesFromLegacyEStopFields();
  loadEStopRoutes(json);
  g_settings.estop_wled_base_url.trim();
  if (g_settings.estop_wled_base_url.endsWith("/")) {
    g_settings.estop_wled_base_url.remove(g_settings.estop_wled_base_url.length() - 1);
  }

  {
    const JsonVariantConst v = json["controlPanelUrl"];
    if (!v.isNull()) {
      g_settings.control_panel_url = v.as<String>();
      g_settings.control_panel_url.trim();
      if (g_settings.control_panel_url.length() == 0) {
        g_settings.control_panel_url = kDefaultControlPanelUrl;
      }
    }
  }

  g_settings.emergency_sources.clear();
  bool hasEmergencySourcesArray = false;
  if (json["emergencySources"].is<JsonArrayConst>()) {
    hasEmergencySourcesArray = true;
    for (JsonVariantConst entry : json["emergencySources"].as<JsonArrayConst>()) {
      if (!entry.is<JsonObjectConst>()) {
        continue;
      }
      const JsonObjectConst srcObj = entry.as<JsonObjectConst>();
      String macText;
      loadString(srcObj, "mac", macText);
      if (macText.length() == 0) {
        continue;
      }
      std::array<uint8_t, 6> parsedMac = {};
      if (!parseMacString(macText, parsedMac)) {
        continue;
      }

      EmergencySourceConfig src = {};
      src.mac = parsedMac;
      src.control_group1_enabled = true;
      src.control_group2_enabled = false;
      src.control_group3_enabled = false;
      loadBool(srcObj, "controlGroup1", src.control_group1_enabled);
      loadBool(srcObj, "controlGroup2", src.control_group2_enabled);
      loadBool(srcObj, "controlGroup3", src.control_group3_enabled);
      g_settings.emergency_sources.push_back(src);
      if (g_settings.emergency_sources.size() >= kMaxEmergencySources) {
        break;
      }
    }
  }

  // Backward-compat loader: old schema used emergencySwitchMacs array.
  if (!hasEmergencySourcesArray && json["emergencySwitchMacs"].is<JsonArrayConst>()) {
    for (JsonVariantConst entry : json["emergencySwitchMacs"].as<JsonArrayConst>()) {
      if (entry.isNull()) {
        continue;
      }
      String macText = entry.as<String>();
      macText.trim();
      if (macText.length() == 0) {
        continue;
      }
      std::array<uint8_t, 6> mac = {};
      if (!parseMacString(macText, mac)) {
        continue;
      }
      EmergencySourceConfig src = {};
      src.mac = mac;
      src.control_group1_enabled = true;
      src.control_group2_enabled = false;
      src.control_group3_enabled = false;
      g_settings.emergency_sources.push_back(src);
      if (g_settings.emergency_sources.size() >= kMaxEmergencySources) {
        break;
      }
    }
  }
}

} // namespace

const AppSettings& getAppSettings() {
  return g_settings;
}

AppSettingsReadGuard::AppSettingsReadGuard() : locked_(false) {
  lockSettings();
  locked_ = true;
}

AppSettingsReadGuard::~AppSettingsReadGuard() {
  if (locked_) {
    unlockSettings();
  }
}

const AppSettings& AppSettingsReadGuard::settings() const {
  return getAppSettings();
}

void appSettingsToJsonImpl(JsonDocument& doc, bool include_pin_code);

namespace app_settings_internal {

void lockSettings() {
  ::lockSettings();
}

void unlockSettings() {
  ::unlockSettings();
}

const char* settingsNvsNamespace() {
  return kSettingsNvsNamespace;
}

const char* settingsNvsKey() {
  return kSettingsNvsKey;
}

const char* defaultControlPanelUrl() {
  return kDefaultControlPanelUrl;
}

void applyDefaultsUnlocked() {
  applyDefaults();
}

void loadFromJsonUnlocked(const JsonObjectConst& json) {
  loadFromJson(json);
}

bool validateControlPanelUrlUnlocked(const String& url) {
  return validateControlPanelUrl(url);
}

void appSettingsToJsonUnlocked(JsonDocument& doc, bool include_pin_code) {
  appSettingsToJsonImpl(doc, include_pin_code);
}

AppSettings& mutableSettings() {
  return g_settings;
}

} // namespace app_settings_internal

String toMacString(const std::array<uint8_t, 6>& mac) {
  char buffer[18];
  snprintf(buffer, sizeof(buffer), "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buffer);
}

bool parseMacString(const String& text, std::array<uint8_t, 6>& out) {
  String normalized = text;
  normalized.trim();
  normalized.toLowerCase();
  normalized.replace("-", ":");

  int values[6];
  if (sscanf(normalized.c_str(), "%x:%x:%x:%x:%x:%x", &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]) != 6) {
    return false;
  }

  for (int i = 0; i < 6; ++i) {
    if (values[i] < 0 || values[i] > 255) {
      return false;
    }
    out[i] = static_cast<uint8_t>(values[i]);
  }
  return true;
}

void appSettingsToJsonImpl(JsonDocument& doc, bool include_pin_code) {
  doc["deviceName"] = g_settings.device_name;

  doc["pinProtectionEnabled"] = g_settings.pin_protection_enabled;
  if (include_pin_code) {
    doc["pinCode"] = g_settings.pin_code;
  }

  doc["runtimeEspNowEnabled"]       = g_settings.runtime_espnow_enabled;
  doc["runtimeMicroRosEnabled"]     = g_settings.runtime_microros_enabled;
  doc["runtimeLedEnabled"]          = g_settings.runtime_led_enabled;
  doc["runtimeSensorEnabled"]       = g_settings.runtime_sensor_enabled;
  doc["controlGroup1Name"]          = g_settings.control_group1_name;
  doc["controlGroup2Name"]          = g_settings.control_group2_name;
  doc["controlGroup3Name"]          = g_settings.control_group3_name;

  doc["controlGroup1SwitchPin"]     = g_settings.control_group1_switch_pin;
  doc["controlGroup2SwitchPin"]     = g_settings.control_group2_switch_pin;
  doc["controlGroup3SwitchPin"]     = g_settings.control_group3_switch_pin;

  doc["controlGroup1PowerPin"]      = g_settings.control_group1_power_pin;
  doc["controlGroup2Power12vPin"]   = g_settings.control_group2_power_12v_pin;
  doc["controlGroup2Power7v4Pin"]   = g_settings.control_group2_power_7v4_pin;
  doc["controlGroup3PowerPin"]      = g_settings.control_group3_power_pin;

  doc["switchActiveHigh"]           = g_settings.switch_active_high;
  doc["powerActiveHigh"]            = g_settings.power_active_high;

  doc["ledPin"]                     = g_settings.led_pin;
  doc["ledCount"]                   = g_settings.led_count;
  doc["ledBrightness"]              = g_settings.led_brightness;
  doc["ledOverrideDurationMs"]      = g_settings.led_override_duration_ms;

  doc["voltmeterPin"]               = g_settings.voltmeter_pin;
  doc["voltageDividerR1"]           = g_settings.voltage_divider_r1;
  doc["voltageDividerR2"]           = g_settings.voltage_divider_r2;
  doc["voltmeterCalibration"]       = g_settings.voltmeter_calibration;
  doc["voltmeterOffset"]            = g_settings.voltmeter_offset;
  doc["slidingWindowSize"]          = g_settings.sliding_window_size;
  doc["timerPeriodUs"]              = g_settings.timer_period_us;
  doc["batteryDisconnectThreshold"] = g_settings.battery_disconnect_threshold;
  doc["batteryLowThreshold"]        = g_settings.battery_low_threshold;

  doc["rosNodeName"]                = g_settings.ros_node_name;
  doc["rosDomainId"]                = g_settings.ros_domain_id;
  doc["rosTimerMs"]                 = g_settings.ros_timer_ms;
  doc["mrosTimeoutMs"]              = g_settings.mros_timeout_ms;
  doc["mrosPingIntervalMs"]         = g_settings.mros_ping_interval_ms;
  doc["espNowChannel"]              = g_settings.espnow_channel;
  doc["estopTargetMac"]             = g_settings.estop_target_mac;
  doc["estopSwitchPin"]             = g_settings.estop_switch_pin;
  doc["estopSwitchActiveHigh"]      = g_settings.estop_switch_active_high;
  doc["estopSwitchLogicInverted"]   = g_settings.estop_switch_logic_inverted;
  doc["estopWledEnabled"]           = g_settings.estop_wled_enabled;
  doc["estopWledBaseUrl"]           = g_settings.estop_wled_base_url;
  doc["estopWledPressedPreset"]     = g_settings.estop_wled_pressed_preset;
  doc["estopWledReleasedPreset"]    = g_settings.estop_wled_released_preset;
  doc["estopBuzzerEnabled"]         = g_settings.estop_buzzer_enabled;
  doc["estopBuzzerPin"]             = g_settings.estop_buzzer_pin;
  JsonArray estopRoutes = doc["estopRoutes"].to<JsonArray>();
  for (const auto& route : g_settings.estop_routes) {
    JsonObject routeObj = estopRoutes.add<JsonObject>();
    routeObj["targetMac"] = route.target_mac;
    routeObj["switchPin"] = route.switch_pin;
    routeObj["switchActiveHigh"] = route.switch_active_high;
    routeObj["switchLogicInverted"] = route.switch_logic_inverted;
  }

  doc["controlPanelUrl"] = g_settings.control_panel_url;

  JsonArray emergencySources = doc["emergencySources"].to<JsonArray>();
  for (const auto& src : g_settings.emergency_sources) {
    JsonObject srcObj = emergencySources.add<JsonObject>();
    srcObj["mac"] = toMacString(src.mac);
    srcObj["controlGroup1"] = src.control_group1_enabled;
    srcObj["controlGroup2"] = src.control_group2_enabled;
    srcObj["controlGroup3"] = src.control_group3_enabled;
  }
}

void appSettingsToJson(JsonDocument& doc, bool include_pin_code) {
  AppSettingsReadGuard guard;
  appSettingsToJsonImpl(doc, include_pin_code);
}
