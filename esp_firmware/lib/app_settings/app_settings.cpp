#include "app_settings.h"
#include "config.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <nvs_flash.h>
#include <cstring>

namespace {

const char* kSettingsNvsNamespace = "espd_cfg";
const char* kSettingsNvsKey = "settings_json";
constexpr size_t   kMaxEmergencySources   = 16;
constexpr size_t   kMaxEStopRoutes        = 16;
constexpr uint8_t  kEspNowChannelMin      = 1;
constexpr uint8_t  kEspNowChannelMax      = 13;
constexpr uint8_t  kEspNowChannelDefault  = 6;

// Align with esp_firmware/data/settings.html min/max (server-side enforcement).
constexpr uint8_t  kGpioPinMaxUi          = 48;
constexpr uint16_t kLedCountMinUi         = 1;
constexpr uint16_t kLedCountMaxUi         = 1024;
constexpr uint32_t kRosTimerMsMin         = 1;
constexpr uint32_t kRosTimerMsMax         = 3600000UL;
constexpr uint32_t kMrosTimeoutMsMin      = 1;
constexpr uint32_t kMrosTimeoutMsMax      = 300000UL;
constexpr uint32_t kMrosPingMsMin         = 10;
constexpr uint32_t kMrosPingMsMax         = 3600000UL;
constexpr uint16_t kSlidingWindowMinUi    = 1;
constexpr uint16_t kSlidingWindowMaxUi    = 2048;
constexpr uint32_t kTimerPeriodUsMinUi    = 1000;
constexpr uint32_t kTimerPeriodUsMaxUi    = 3600000000UL;
constexpr uint32_t kLedOverrideMsMaxUi    = 604800000UL;
constexpr float kBatteryVoltageAbsMaxUi   = 60.0f;
constexpr float kVoltmeterOffsetAbsMaxUi  = 25.0f;
constexpr uint16_t kWledPresetMin         = 1;
constexpr uint16_t kWledPresetMax         = 250;
constexpr size_t kWledUrlMaxLen           = 192;
constexpr size_t kControlGroupNameMaxLen  = 32;
constexpr const char* kDefaultControlGroupName1 = "Chassis Power";
constexpr const char* kDefaultControlGroupName2 = "Actuators Power";
constexpr const char* kDefaultControlGroupName3 = "Others Power";

/** Default status-page Control Panel shortcut when unset or empty in JSON. */
constexpr const char* kDefaultControlPanelUrl = "https://scx.tw/links";

AppSettings g_settings;

EStopRouteConfig buildDefaultEStopRoute() {
  EStopRouteConfig route = {};
  route.target_mac = "";
  route.switch_pin = 4;
  route.switch_active_high = false;
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
    g_settings.estop_target_mac = route.target_mac;
    g_settings.estop_switch_pin = route.switch_pin;
    g_settings.estop_switch_active_high = route.switch_active_high;
    g_settings.estop_switch_logic_inverted = route.switch_logic_inverted;
    return;
  }

  const EStopRouteConfig& primary = g_settings.estop_routes.front();
  g_settings.estop_target_mac = primary.target_mac;
  g_settings.estop_switch_pin = primary.switch_pin;
  g_settings.estop_switch_active_high = primary.switch_active_high;
  g_settings.estop_switch_logic_inverted = primary.switch_logic_inverted;
}

void syncRoutesFromLegacyEStopFields() {
  g_settings.estop_routes.clear();

  EStopRouteConfig route = {};
  route.target_mac = g_settings.estop_target_mac;
  route.switch_pin = g_settings.estop_switch_pin;
  route.switch_active_high = g_settings.estop_switch_active_high;
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

bool isDigitsOnly(const String& value) {
  if (value.length() == 0) {
    return false;
  }
  for (size_t i = 0; i < value.length(); ++i) {
    const char ch = value[i];
    if (ch < '0' || ch > '9') {
      return false;
    }
  }
  return true;
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
  g_settings.device_name = defaultDeviceNameFromMac();

  g_settings.pin_protection_enabled = false;
  g_settings.pin_code = "1234";

#if APP_MODE == APP_MODE_ESTOP
  g_settings.runtime_espnow_enabled = true;
  g_settings.runtime_microros_enabled = false;
  g_settings.runtime_led_enabled = false;
  g_settings.runtime_sensor_enabled = false;
#else
  g_settings.runtime_espnow_enabled = true;
  g_settings.runtime_microros_enabled = true;
  g_settings.runtime_led_enabled = true;
  g_settings.runtime_sensor_enabled = true;
#endif

  g_settings.control_group1_name = kDefaultControlGroupName1;
  g_settings.control_group2_name = kDefaultControlGroupName2;
  g_settings.control_group3_name = kDefaultControlGroupName3;

  g_settings.control_group1_switch_pin = 8;
  g_settings.control_group2_switch_pin = 9;
  g_settings.control_group3_switch_pin = 20;

  g_settings.control_group1_power_pin = 4;
  g_settings.control_group2_power_12v_pin = 3;
  g_settings.control_group2_power_7v4_pin = 5;
  g_settings.control_group3_power_pin = 10;

  g_settings.switch_active_high = false;
  g_settings.power_active_high = false;

  g_settings.led_pin = 6;
  g_settings.led_count = 40;
  g_settings.led_brightness = 200;
  g_settings.led_override_duration_ms = 1000;

  g_settings.voltmeter_pin = 2;
  g_settings.voltage_divider_r1 = 47000.0f;
  g_settings.voltage_divider_r2 = 4700.0f;
  g_settings.voltmeter_calibration = 11.0f;
  g_settings.voltmeter_offset = 0.0f;
  g_settings.sliding_window_size = 64;
  g_settings.timer_period_us = 1000000;
  g_settings.battery_disconnect_threshold = 3.0f;
  g_settings.battery_low_threshold = 17.5f;
  updateCalibrationFromDivider();

  g_settings.ros_node_name = "esp_daemon";
  g_settings.ros_domain_id = 0;
  g_settings.ros_timer_ms = 100;
  g_settings.mros_timeout_ms = 100;
  g_settings.mros_ping_interval_ms = 1000;
  g_settings.espnow_channel = kEspNowChannelDefault;
  g_settings.estop_target_mac = "";
  g_settings.estop_switch_pin = 4;
  g_settings.estop_switch_active_high = false;
  g_settings.estop_switch_logic_inverted = false;
  g_settings.estop_wled_enabled = false;
  g_settings.estop_wled_base_url = "";
  g_settings.estop_wled_preset = 1;
  g_settings.estop_buzzer_enabled = false;
  g_settings.estop_buzzer_pin = 5;
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
  loadUInt16(json, "estopWledPreset",               g_settings.estop_wled_preset);
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

void appSettingsToJson(JsonDocument& doc, bool include_pin_code) {
  syncLegacyEStopFieldsFromRoutes();

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
  doc["estopWledPreset"]            = g_settings.estop_wled_preset;
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

bool saveAppSettings() {
  JsonDocument doc;
  appSettingsToJson(doc, true);

  String payload;
  if (serializeJson(doc, payload) == 0 || payload.length() == 0) {
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(kSettingsNvsNamespace, false)) {
    return false;
  }
  const size_t written = prefs.putString(kSettingsNvsKey, payload);
  prefs.end();
  return written > 0;
}

bool resetAppSettingsToDefaults() {
  applyDefaults();
  return saveAppSettings();
}

bool eraseAppSettingsFromNvs() {
  const esp_err_t deinitErr = nvs_flash_deinit();
  if (deinitErr != ESP_OK && deinitErr != ESP_ERR_NVS_NOT_INITIALIZED) {
    return false;
  }

  if (nvs_flash_erase() != ESP_OK) {
    return false;
  }

  if (nvs_flash_init() != ESP_OK) {
    return false;
  }

  // Keep runtime consistent with wiped storage until next reboot.
  // Note: this now erases the whole NVS partition (including Wi-Fi creds).
  applyDefaults();
  return true;
}

void initAppSettings(bool storage_available) {
  (void)storage_available;
  applyDefaults();
  bool loaded = false;

  // Primary storage: NVS (Preferences). Survives SPIFFS uploads.
  Preferences prefs;
  if (prefs.begin(kSettingsNvsNamespace, true)) {
    const String payload = prefs.getString(kSettingsNvsKey, "");
    prefs.end();

    if (payload.length() > 0) {
      JsonDocument doc;
      const DeserializationError err = deserializeJson(doc, payload);
      if (err) {
        DAEMON_LOGF("Failed to parse NVS settings (%s), using defaults\n", err.c_str());
      } else if (!doc.is<JsonObject>()) {
        DAEMON_LOGLN("Invalid NVS settings root, using defaults");
      } else {
        loadFromJson(doc.as<JsonObjectConst>());
        loaded = true;
      }
    } else {
      DAEMON_LOGLN("NVS settings missing, using defaults");
    }
  } else {
    DAEMON_LOGLN("Failed to open NVS settings namespace, using defaults");
  }

  if (!loaded) {
    DAEMON_LOGLN("Using in-memory default settings");
  }

  if (!validateControlPanelUrl(g_settings.control_panel_url)) {
    g_settings.control_panel_url = kDefaultControlPanelUrl;
  }
}

bool updateAppSettingsFromJson(const JsonObjectConst& json, String& error) {
  (void)error;
  AppSettings backup = g_settings;

  loadFromJson(json);

  // If the UI sends an empty pinCode, treat it as "no update" and keep the
  // previously stored PIN. This prevents save failures when pinCode is not
  // included in /settings/read (the input will be empty in the UI).
  {
    const JsonVariantConst v = json["pinCode"];
    if (!v.isNull()) {
      String candidate = v.as<String>();
      candidate.trim();
      if (candidate.length() == 0) {
        g_settings.pin_code = backup.pin_code;
      }
    }
  }

  if (g_settings.control_panel_url.length() == 0) {
    g_settings.control_panel_url = kDefaultControlPanelUrl;
  }

  if (g_settings.device_name.length() == 0) {
    g_settings = backup;
    error = "deviceName cannot be empty";
    return false;
  }

  // WiFi hostname / mDNS label are DNS labels and must be <= 63 characters.
  // The UI accepts any casing, but the backend sanitizes and lowercases.
  if (g_settings.device_name.length() > 63) {
    g_settings = backup;
    error = "deviceName must be <= 63 characters";
    return false;
  }

  if (g_settings.control_group1_name.length() > kControlGroupNameMaxLen) {
    g_settings = backup;
    error = "controlGroup1Name must be <= 32 characters";
    return false;
  }
  if (g_settings.control_group2_name.length() > kControlGroupNameMaxLen) {
    g_settings = backup;
    error = "controlGroup2Name must be <= 32 characters";
    return false;
  }
  if (g_settings.control_group3_name.length() > kControlGroupNameMaxLen) {
    g_settings = backup;
    error = "controlGroup3Name must be <= 32 characters";
    return false;
  }

  if (g_settings.pin_protection_enabled) {
    g_settings.pin_code.trim();

    if (g_settings.pin_code.length() < 4) {
      g_settings = backup;
      error = "pinCode must be at least 4 digits when protection is enabled";
      return false;
    }

    if (g_settings.pin_code.length() > 32) {
      g_settings = backup;
      error = "pinCode must be at most 32 digits";
      return false;
    }

    if (!isDigitsOnly(g_settings.pin_code)) {
      g_settings = backup;
      error = "pinCode must contain only digits (0-9)";
      return false;
    }
  }

  if (g_settings.ros_domain_id < 0 || g_settings.ros_domain_id > 232) {
    g_settings = backup;
    error = "rosDomainId must be in range 0..232";
    return false;
  }

  if (g_settings.espnow_channel < kEspNowChannelMin || g_settings.espnow_channel > kEspNowChannelMax) {
    g_settings = backup;
    error = "espNowChannel must be in range 1..13";
    return false;
  }

#if APP_MODE == APP_MODE_ESTOP
  if (g_settings.estop_routes.empty()) {
    g_settings = backup;
    error = "estopRoutes must contain at least 1 route";
    return false;
  }

  if (g_settings.estop_routes.size() > kMaxEStopRoutes) {
    g_settings = backup;
    error = "estopRoutes maximum is 16";
    return false;
  }

  for (size_t i = 0; i < g_settings.estop_routes.size(); ++i) {
    EStopRouteConfig& route = g_settings.estop_routes[i];

    if (route.switch_pin > kGpioPinMaxUi) {
      g_settings = backup;
      error = "estopRoutes[" + String(i) + "].switchPin must be in range 0..48";
      return false;
    }

    route.target_mac.replace("-", ":");
    route.target_mac.trim();
    route.target_mac.toLowerCase();
    if (route.target_mac.length() > 0) {
      std::array<uint8_t, 6> parsedMac = {};
      if (!parseMacString(route.target_mac, parsedMac)) {
        g_settings = backup;
        error = "estopRoutes[" + String(i) + "].targetMac must be a valid MAC address";
        return false;
      }
      route.target_mac = toMacString(parsedMac);
    }
  }

  for (size_t i = 0; i < g_settings.estop_routes.size(); ++i) {
    for (size_t j = i + 1; j < g_settings.estop_routes.size(); ++j) {
      const EStopRouteConfig& a = g_settings.estop_routes[i];
      const EStopRouteConfig& b = g_settings.estop_routes[j];
      if (a.switch_pin == b.switch_pin &&
          a.switch_active_high == b.switch_active_high &&
          a.switch_logic_inverted == b.switch_logic_inverted &&
          a.target_mac == b.target_mac) {
        g_settings = backup;
        error = "Duplicate estopRoutes entries at index " + String(i) + " and " + String(j);
        return false;
      }
    }
  }

  syncLegacyEStopFieldsFromRoutes();

  if (g_settings.estop_wled_base_url.endsWith("/")) {
    g_settings.estop_wled_base_url.remove(g_settings.estop_wled_base_url.length() - 1);
  }

  if (g_settings.estop_wled_base_url.length() > kWledUrlMaxLen) {
    g_settings = backup;
    error = "estopWledBaseUrl must be <= 192 chars";
    return false;
  }

  if (g_settings.estop_wled_enabled) {
    if (g_settings.estop_wled_base_url.length() == 0) {
      g_settings = backup;
      error = "estopWledBaseUrl is required when estopWledEnabled=true";
      return false;
    }
    if (!(g_settings.estop_wled_base_url.startsWith("http://") ||
          g_settings.estop_wled_base_url.startsWith("https://"))) {
      g_settings = backup;
      error = "estopWledBaseUrl must start with http:// or https://";
      return false;
    }
  }

  if (g_settings.estop_wled_preset < kWledPresetMin || g_settings.estop_wled_preset > kWledPresetMax) {
    g_settings = backup;
    error = "estopWledPreset must be in range 1..250";
    return false;
  }

  if (g_settings.estop_buzzer_pin > kGpioPinMaxUi) {
    g_settings = backup;
    error = "estopBuzzerPin must be in range 0..48";
    return false;
  }

  if (g_settings.estop_buzzer_enabled) {
    for (size_t i = 0; i < g_settings.estop_routes.size(); ++i) {
      if (g_settings.estop_routes[i].switch_pin == g_settings.estop_buzzer_pin) {
        g_settings = backup;
        error = "estopBuzzerPin conflicts with estopRoutes[" + String(i) + "].switchPin";
        return false;
      }
    }
  }
#else
  if (g_settings.battery_low_threshold < g_settings.battery_disconnect_threshold) {
    g_settings = backup;
    error = "batteryLowThreshold must be >= batteryDisconnectThreshold";
    return false;
  }

  if (!validateControlPanelUrl(g_settings.control_panel_url)) {
    g_settings = backup;
    error = "controlPanelUrl must start with / or http:// or https:// (max 192 chars)";
    return false;
  }

  if (g_settings.emergency_sources.size() > kMaxEmergencySources) {
    g_settings = backup;
    error = "emergencySources maximum is 16";
    return false;
  }

  for (size_t i = 0; i < g_settings.emergency_sources.size(); ++i) {
    const EmergencySourceConfig& src = g_settings.emergency_sources[i];
    if (!src.control_group1_enabled &&
        !src.control_group2_enabled &&
        !src.control_group3_enabled) {
      g_settings = backup;
      error = "emergencySources[" + String(i) + "] must target at least one control group";
      return false;
    }
  }

  for (size_t i = 0; i < g_settings.emergency_sources.size(); ++i) {
    for (size_t j = i + 1; j < g_settings.emergency_sources.size(); ++j) {
      if (memcmp(g_settings.emergency_sources[i].mac.data(),
                 g_settings.emergency_sources[j].mac.data(), 6) == 0) {
        g_settings = backup;
        error = "Duplicate emergencySources MAC at index " + String(i) + " and " + String(j);
        return false;
      }
    }
  }

  if (g_settings.voltage_divider_r1 < 0.0f || g_settings.voltage_divider_r2 <= 0.0f) {
    g_settings = backup;
    error = "voltageDividerR1 must be >= 0 and voltageDividerR2 must be > 0";
    return false;
  }

  if (!(g_settings.voltmeter_calibration > 0.0f)) {
    g_settings = backup;
    error = "voltmeterCalibration must be > 0";
    return false;
  }

#define GPIO_CHECK(name, field)                                                                                      \
  if ((field) > kGpioPinMaxUi) {                                                                                     \
    g_settings = backup;                                                                                             \
    error = String(name) + " must be in range 0.." + String(kGpioPinMaxUi);                                          \
    return false;                                                                                                    \
  }

  GPIO_CHECK("controlGroup1SwitchPin",   g_settings.control_group1_switch_pin);
  GPIO_CHECK("controlGroup2SwitchPin",   g_settings.control_group2_switch_pin);
  GPIO_CHECK("controlGroup3SwitchPin",   g_settings.control_group3_switch_pin);
  GPIO_CHECK("controlGroup1PowerPin",    g_settings.control_group1_power_pin);
  GPIO_CHECK("controlGroup2Power12vPin", g_settings.control_group2_power_12v_pin);
  GPIO_CHECK("controlGroup2Power7v4Pin", g_settings.control_group2_power_7v4_pin);
  GPIO_CHECK("controlGroup3PowerPin",    g_settings.control_group3_power_pin);
  GPIO_CHECK("ledPin",                g_settings.led_pin);
  GPIO_CHECK("voltmeterPin",          g_settings.voltmeter_pin);
#undef GPIO_CHECK

  // Prevent accidental GPIO reuse across different roles (switch/power/led/voltmeter).
  // Reusing the same physical pin for multiple tasks can cause conflicting electrical behavior.
  {
    struct PinItem {
      const char* name;
      uint32_t value;
    };

    const std::array<PinItem, 9> pins = {{
      {"controlGroup1SwitchPin",   g_settings.control_group1_switch_pin},
      {"controlGroup2SwitchPin",   g_settings.control_group2_switch_pin},
      {"controlGroup3SwitchPin",   g_settings.control_group3_switch_pin},
      {"controlGroup1PowerPin",    g_settings.control_group1_power_pin},
      {"controlGroup2Power12vPin", g_settings.control_group2_power_12v_pin},
      {"controlGroup2Power7v4Pin", g_settings.control_group2_power_7v4_pin},
      {"controlGroup3PowerPin",    g_settings.control_group3_power_pin},
      {"ledPin",                g_settings.led_pin},
      {"voltmeterPin",          g_settings.voltmeter_pin},
    }};

    for (size_t i = 0; i < pins.size(); ++i) {
      for (size_t j = i + 1; j < pins.size(); ++j) {
        if (pins[i].value != pins[j].value) {
          continue;
        }
        error = String("Duplicate GPIO pin: ") +
                pins[i].name + " and " + pins[j].name +
                " both use GPIO " + String(pins[i].value);
        return false;
      }
    }
  }

  if (g_settings.led_count < kLedCountMinUi || g_settings.led_count > kLedCountMaxUi) {
    g_settings = backup;
    error = "ledCount must be in range 1..1024";
    return false;
  }

  if (g_settings.sliding_window_size < kSlidingWindowMinUi || g_settings.sliding_window_size > kSlidingWindowMaxUi) {
    g_settings = backup;
    error = "slidingWindowSize must be in range 1..2048";
    return false;
  }

  if (g_settings.ros_timer_ms < kRosTimerMsMin || g_settings.ros_timer_ms > kRosTimerMsMax) {
    g_settings = backup;
    error = "rosTimerMs must be in range 1..3600000";
    return false;
  }

  if (g_settings.mros_timeout_ms < kMrosTimeoutMsMin || g_settings.mros_timeout_ms > kMrosTimeoutMsMax) {
    g_settings = backup;
    error = "mrosTimeoutMs must be in range 1..300000";
    return false;
  }

  if (g_settings.mros_ping_interval_ms < kMrosPingMsMin || g_settings.mros_ping_interval_ms > kMrosPingMsMax) {
    g_settings = backup;
    error = "mrosPingIntervalMs must be in range 10..3600000";
    return false;
  }

  if (g_settings.timer_period_us < kTimerPeriodUsMinUi || g_settings.timer_period_us > kTimerPeriodUsMaxUi) {
    g_settings = backup;
    error = "timerPeriodUs must be in range 1000..3600000000";
    return false;
  }

  if (g_settings.led_override_duration_ms > kLedOverrideMsMaxUi) {
    g_settings = backup;
    error = "ledOverrideDurationMs must be <= 604800000 (7 days)";
    return false;
  }

  if (g_settings.battery_disconnect_threshold < 0.0f || g_settings.battery_disconnect_threshold > kBatteryVoltageAbsMaxUi) {
    g_settings = backup;
    error = "batteryDisconnectThreshold must be in range 0..60 V";
    return false;
  }

  if (g_settings.battery_low_threshold < 0.0f || g_settings.battery_low_threshold > kBatteryVoltageAbsMaxUi) {
    g_settings = backup;
    error = "batteryLowThreshold must be in range 0..60 V";
    return false;
  }

  if (g_settings.voltmeter_offset < -kVoltmeterOffsetAbsMaxUi || g_settings.voltmeter_offset > kVoltmeterOffsetAbsMaxUi) {
    g_settings = backup;
    error = "voltmeterOffset must be in range -25..25 V";
    return false;
  }
#endif

  if (!saveAppSettings()) {
    g_settings = backup;
    error = "failed to save settings";
    return false;
  }

  return true;
}

bool verifySettingsPin(const String& pin) {
  if (!g_settings.pin_protection_enabled) {
    return true;
  }
  String a = pin;
  a.trim();
  String b = g_settings.pin_code;
  b.trim();
  return a == b;
}
