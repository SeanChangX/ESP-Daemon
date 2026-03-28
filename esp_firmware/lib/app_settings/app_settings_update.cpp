#include "app_settings_internal.h"
#include "config.h"

#include <array>
#include <cstring>

#ifndef __has_include
#define __has_include(x) 0
#endif
#if __has_include(<rmw/validate_node_name.h>)
#include <rmw/validate_node_name.h>
#define ESP_DAEMON_HAS_RMW_NODE_NAME_VALIDATOR 1
#else
#define ESP_DAEMON_HAS_RMW_NODE_NAME_VALIDATOR 0
#endif

namespace {

class ScopedSettingsWriteLock {
public:
  ScopedSettingsWriteLock() {
    app_settings_internal::lockSettings();
  }

  ~ScopedSettingsWriteLock() {
    app_settings_internal::unlockSettings();
  }

  ScopedSettingsWriteLock(const ScopedSettingsWriteLock&) = delete;
  ScopedSettingsWriteLock& operator=(const ScopedSettingsWriteLock&) = delete;
};

constexpr size_t   kMaxEmergencySources     = 16;
constexpr size_t   kMaxEStopRoutes          = 16;
constexpr uint8_t  kEspNowChannelMin        = 1;
constexpr uint8_t  kEspNowChannelMax        = 13;
constexpr uint8_t  kGpioPinMaxUi            = 48;
constexpr uint16_t kLedCountMinUi           = 1;
constexpr uint16_t kLedCountMaxUi           = 1024;
constexpr uint32_t kRosTimerMsMin           = 1;
constexpr uint32_t kRosTimerMsMax           = 3600000UL;
constexpr uint32_t kMrosTimeoutMsMin        = 1;
constexpr uint32_t kMrosTimeoutMsMax        = 300000UL;
constexpr uint32_t kMrosPingMsMin           = 10;
constexpr uint32_t kMrosPingMsMax           = 3600000UL;
constexpr uint16_t kSlidingWindowMinUi      = 1;
constexpr uint16_t kSlidingWindowMaxUi      = 2048;
constexpr uint32_t kTimerPeriodUsMinUi      = 1000;
constexpr uint32_t kTimerPeriodUsMaxUi      = 3600000000UL;
constexpr uint32_t kLedOverrideMsMaxUi      = 604800000UL;
constexpr float    kBatteryVoltageAbsMaxUi  = 60.0f;
constexpr float    kVoltmeterOffsetAbsMaxUi = 25.0f;
constexpr uint16_t kWledPresetMin           = 1;
constexpr uint16_t kWledPresetMax           = 250;
constexpr size_t   kWledUrlMaxLen           = 192;
constexpr size_t   kControlGroupNameMaxLen  = 32;
constexpr size_t   kRosNodeNameMaxLen       = 255;

bool isDigitsOnlyLocal(const String& value) {
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

#if !ESP_DAEMON_HAS_RMW_NODE_NAME_VALIDATOR
bool isAsciiAlphaLocal(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

bool isAsciiDigitLocal(char ch) {
  return ch >= '0' && ch <= '9';
}
#endif

bool isValidRosNodeNameLocal(const String& value) {
  if (value.length() == 0) {
    return false;
  }

#if ESP_DAEMON_HAS_RMW_NODE_NAME_VALIDATOR
  int validation_result = RMW_NODE_NAME_VALID;
  size_t invalid_index = 0;
  const rmw_ret_t rc = rmw_validate_node_name(value.c_str(), &validation_result, &invalid_index);
  (void)invalid_index;
  return (rc == RMW_RET_OK) && (validation_result == RMW_NODE_NAME_VALID);
#else
  const char first = value[0];
  if (!(isAsciiAlphaLocal(first) || first == '_')) {
    return false;
  }

  for (size_t i = 1; i < value.length(); ++i) {
    const char ch = value[i];
    if (!(isAsciiAlphaLocal(ch) || isAsciiDigitLocal(ch) || ch == '_')) {
      return false;
    }
  }

  return true;
#endif
}

void syncLegacyEStopFieldsFromRoutesLocal(AppSettings& settings) {
  if (settings.estop_routes.empty()) {
    settings.estop_target_mac = "";
    settings.estop_switch_pin = 4;
    settings.estop_switch_active_high = false;
    settings.estop_switch_logic_inverted = false;
    return;
  }

  const EStopRouteConfig& primary = settings.estop_routes.front();
  settings.estop_target_mac = primary.target_mac;
  settings.estop_switch_pin = primary.switch_pin;
  settings.estop_switch_active_high = primary.switch_active_high;
  settings.estop_switch_logic_inverted = primary.switch_logic_inverted;
}

} // namespace

bool updateAppSettingsFromJson(const JsonObjectConst& json, String& error) {
  ScopedSettingsWriteLock guard;
  AppSettings& settings = app_settings_internal::mutableSettings();
  (void)error;
  AppSettings backup = settings;

  app_settings_internal::loadFromJsonUnlocked(json);

  // If the UI sends an empty pinCode, treat it as "no update" and keep the
  // previously stored PIN. This prevents save failures when pinCode is not
  // included in /settings/read (the input will be empty in the UI).
  {
    const JsonVariantConst v = json["pinCode"];
    if (!v.isNull()) {
      String candidate = v.as<String>();
      candidate.trim();
      if (candidate.length() == 0) {
        settings.pin_code = backup.pin_code;
      }
    }
  }

  if (settings.control_panel_url.length() == 0) {
    settings.control_panel_url = app_settings_internal::defaultControlPanelUrl();
  }

  if (settings.device_name.length() == 0) {
    settings = backup;
    error = "deviceName cannot be empty";
    return false;
  }

  // WiFi hostname / mDNS label are DNS labels and must be <= 63 characters.
  // The UI accepts any casing, but the backend sanitizes and lowercases.
  if (settings.device_name.length() > 63) {
    settings = backup;
    error = "deviceName must be <= 63 characters";
    return false;
  }

  if (settings.control_group1_name.length() > kControlGroupNameMaxLen) {
    settings = backup;
    error = "controlGroup1Name must be <= 32 characters";
    return false;
  }
  if (settings.control_group2_name.length() > kControlGroupNameMaxLen) {
    settings = backup;
    error = "controlGroup2Name must be <= 32 characters";
    return false;
  }
  if (settings.control_group3_name.length() > kControlGroupNameMaxLen) {
    settings = backup;
    error = "controlGroup3Name must be <= 32 characters";
    return false;
  }

  if (settings.pin_protection_enabled) {
    settings.pin_code.trim();

    if (settings.pin_code.length() < 4) {
      settings = backup;
      error = "pinCode must be at least 4 digits when protection is enabled";
      return false;
    }

    if (settings.pin_code.length() > 32) {
      settings = backup;
      error = "pinCode must be at most 32 digits";
      return false;
    }

    if (!isDigitsOnlyLocal(settings.pin_code)) {
      settings = backup;
      error = "pinCode must contain only digits (0-9)";
      return false;
    }
  }

  if (settings.ros_domain_id < 0 || settings.ros_domain_id > 232) {
    settings = backup;
    error = "rosDomainId must be in range 0..232";
    return false;
  }

  settings.ros_node_name.trim();
  if (settings.ros_node_name.length() > kRosNodeNameMaxLen) {
    settings = backup;
    error = "rosNodeName must be <= 255 characters";
    return false;
  }
  if (!isValidRosNodeNameLocal(settings.ros_node_name)) {
    settings = backup;
    error = "rosNodeName is invalid for ROS 2 (use letters/numbers/_ and do not start with a digit)";
    return false;
  }

  if (settings.espnow_channel < kEspNowChannelMin || settings.espnow_channel > kEspNowChannelMax) {
    settings = backup;
    error = "espNowChannel must be in range 1..13";
    return false;
  }

#if APP_MODE == APP_MODE_ESTOP
  if (settings.estop_routes.empty()) {
    settings = backup;
    error = "estopRoutes must contain at least 1 route";
    return false;
  }

  if (settings.estop_routes.size() > kMaxEStopRoutes) {
    settings = backup;
    error = "estopRoutes maximum is 16";
    return false;
  }

  for (size_t i = 0; i < settings.estop_routes.size(); ++i) {
    EStopRouteConfig& route = settings.estop_routes[i];

    if (route.switch_pin > kGpioPinMaxUi) {
      settings = backup;
      error = "estopRoutes[" + String(i) + "].switchPin must be in range 0..48";
      return false;
    }

    route.target_mac.replace("-", ":");
    route.target_mac.trim();
    route.target_mac.toLowerCase();
    if (route.target_mac.length() > 0) {
      std::array<uint8_t, 6> parsedMac = {};
      if (!parseMacString(route.target_mac, parsedMac)) {
        settings = backup;
        error = "estopRoutes[" + String(i) + "].targetMac must be a valid MAC address";
        return false;
      }
      route.target_mac = toMacString(parsedMac);
    }
  }

  for (size_t i = 0; i < settings.estop_routes.size(); ++i) {
    for (size_t j = i + 1; j < settings.estop_routes.size(); ++j) {
      const EStopRouteConfig& a = settings.estop_routes[i];
      const EStopRouteConfig& b = settings.estop_routes[j];
      if (a.switch_pin == b.switch_pin &&
          a.switch_active_high == b.switch_active_high &&
          a.switch_logic_inverted == b.switch_logic_inverted &&
          a.target_mac == b.target_mac) {
        settings = backup;
        error = "Duplicate estopRoutes entries at index " + String(i) + " and " + String(j);
        return false;
      }
    }
  }

  syncLegacyEStopFieldsFromRoutesLocal(settings);

  if (settings.estop_wled_base_url.endsWith("/")) {
    settings.estop_wled_base_url.remove(settings.estop_wled_base_url.length() - 1);
  }

  if (settings.estop_wled_base_url.length() > kWledUrlMaxLen) {
    settings = backup;
    error = "estopWledBaseUrl must be <= 192 chars";
    return false;
  }

  if (settings.estop_wled_enabled) {
    if (settings.estop_wled_base_url.length() == 0) {
      settings = backup;
      error = "estopWledBaseUrl is required when estopWledEnabled=true";
      return false;
    }
    if (!(settings.estop_wled_base_url.startsWith("http://") ||
          settings.estop_wled_base_url.startsWith("https://"))) {
      settings = backup;
      error = "estopWledBaseUrl must start with http:// or https://";
      return false;
    }
  }

  if (settings.estop_wled_pressed_preset < kWledPresetMin || settings.estop_wled_pressed_preset > kWledPresetMax) {
    settings = backup;
    error = "estopWledPressedPreset must be in range 1..250";
    return false;
  }

  if (settings.estop_wled_released_preset < kWledPresetMin || settings.estop_wled_released_preset > kWledPresetMax) {
    settings = backup;
    error = "estopWledReleasedPreset must be in range 1..250";
    return false;
  }

  if (settings.estop_buzzer_pin > kGpioPinMaxUi) {
    settings = backup;
    error = "estopBuzzerPin must be in range 0..48";
    return false;
  }

  if (settings.estop_buzzer_enabled) {
    for (size_t i = 0; i < settings.estop_routes.size(); ++i) {
      if (settings.estop_routes[i].switch_pin == settings.estop_buzzer_pin) {
        settings = backup;
        error = "estopBuzzerPin conflicts with estopRoutes[" + String(i) + "].switchPin";
        return false;
      }
    }
  }
#else
  if (settings.battery_low_threshold < settings.battery_disconnect_threshold) {
    settings = backup;
    error = "batteryLowThreshold must be >= batteryDisconnectThreshold";
    return false;
  }

  if (!app_settings_internal::validateControlPanelUrlUnlocked(settings.control_panel_url)) {
    settings = backup;
    error = "controlPanelUrl must start with / or http:// or https:// (max 192 chars)";
    return false;
  }

  if (settings.emergency_sources.size() > kMaxEmergencySources) {
    settings = backup;
    error = "emergencySources maximum is 16";
    return false;
  }

  for (size_t i = 0; i < settings.emergency_sources.size(); ++i) {
    const EmergencySourceConfig& src = settings.emergency_sources[i];
    if (!src.control_group1_enabled &&
        !src.control_group2_enabled &&
        !src.control_group3_enabled) {
      settings = backup;
      error = "emergencySources[" + String(i) + "] must target at least one control group";
      return false;
    }
  }

  for (size_t i = 0; i < settings.emergency_sources.size(); ++i) {
    for (size_t j = i + 1; j < settings.emergency_sources.size(); ++j) {
      if (memcmp(settings.emergency_sources[i].mac.data(),
                 settings.emergency_sources[j].mac.data(), 6) == 0) {
        settings = backup;
        error = "Duplicate emergencySources MAC at index " + String(i) + " and " + String(j);
        return false;
      }
    }
  }

  if (settings.voltage_divider_r1 < 0.0f || settings.voltage_divider_r2 <= 0.0f) {
    settings = backup;
    error = "voltageDividerR1 must be >= 0 and voltageDividerR2 must be > 0";
    return false;
  }

  if (!(settings.voltmeter_calibration > 0.0f)) {
    settings = backup;
    error = "voltmeterCalibration must be > 0";
    return false;
  }

#define GPIO_CHECK(name, field)                                                                                      \
  if ((field) > kGpioPinMaxUi) {                                                                                     \
    settings = backup;                                                                                             \
    error = String(name) + " must be in range 0.." + String(kGpioPinMaxUi);                                          \
    return false;                                                                                                    \
  }

  GPIO_CHECK("controlGroup1SwitchPin",   settings.control_group1_switch_pin);
  GPIO_CHECK("controlGroup2SwitchPin",   settings.control_group2_switch_pin);
  GPIO_CHECK("controlGroup3SwitchPin",   settings.control_group3_switch_pin);
  GPIO_CHECK("controlGroup1PowerPin",    settings.control_group1_power_pin);
  GPIO_CHECK("controlGroup2Power12vPin", settings.control_group2_power_12v_pin);
  GPIO_CHECK("controlGroup2Power7v4Pin", settings.control_group2_power_7v4_pin);
  GPIO_CHECK("controlGroup3PowerPin",    settings.control_group3_power_pin);
  GPIO_CHECK("ledPin",                   settings.led_pin);
  GPIO_CHECK("voltmeterPin",             settings.voltmeter_pin);
#undef GPIO_CHECK

  // Prevent accidental GPIO reuse across different roles (switch/power/led/voltmeter).
  // Reusing the same physical pin for multiple tasks can cause conflicting electrical behavior.
  {
    struct PinItem {
      const char* name;
      uint32_t value;
    };

    const std::array<PinItem, 9> pins = {{
      {"controlGroup1SwitchPin",   settings.control_group1_switch_pin},
      {"controlGroup2SwitchPin",   settings.control_group2_switch_pin},
      {"controlGroup3SwitchPin",   settings.control_group3_switch_pin},
      {"controlGroup1PowerPin",    settings.control_group1_power_pin},
      {"controlGroup2Power12vPin", settings.control_group2_power_12v_pin},
      {"controlGroup2Power7v4Pin", settings.control_group2_power_7v4_pin},
      {"controlGroup3PowerPin",    settings.control_group3_power_pin},
      {"ledPin",                   settings.led_pin},
      {"voltmeterPin",             settings.voltmeter_pin},
    }};

    for (size_t i = 0; i < pins.size(); ++i) {
      for (size_t j = i + 1; j < pins.size(); ++j) {
        if (pins[i].value != pins[j].value) {
          continue;
        }
        settings = backup;
        error = String("Duplicate GPIO pin: ") +
                pins[i].name + " and " + pins[j].name +
                " both use GPIO " + String(pins[i].value);
        return false;
      }
    }
  }

  if (settings.led_count < kLedCountMinUi || settings.led_count > kLedCountMaxUi) {
    settings = backup;
    error = "ledCount must be in range 1..1024";
    return false;
  }

  if (settings.sliding_window_size < kSlidingWindowMinUi || settings.sliding_window_size > kSlidingWindowMaxUi) {
    settings = backup;
    error = "slidingWindowSize must be in range 1..2048";
    return false;
  }

  if (settings.ros_timer_ms < kRosTimerMsMin || settings.ros_timer_ms > kRosTimerMsMax) {
    settings = backup;
    error = "rosTimerMs must be in range 1..3600000";
    return false;
  }

  if (settings.mros_timeout_ms < kMrosTimeoutMsMin || settings.mros_timeout_ms > kMrosTimeoutMsMax) {
    settings = backup;
    error = "mrosTimeoutMs must be in range 1..300000";
    return false;
  }

  if (settings.mros_ping_interval_ms < kMrosPingMsMin || settings.mros_ping_interval_ms > kMrosPingMsMax) {
    settings = backup;
    error = "mrosPingIntervalMs must be in range 10..3600000";
    return false;
  }

  if (settings.timer_period_us < kTimerPeriodUsMinUi || settings.timer_period_us > kTimerPeriodUsMaxUi) {
    settings = backup;
    error = "timerPeriodUs must be in range 1000..3600000000";
    return false;
  }

  if (settings.led_override_duration_ms > kLedOverrideMsMaxUi) {
    settings = backup;
    error = "ledOverrideDurationMs must be <= 604800000 (7 days)";
    return false;
  }

  if (settings.battery_disconnect_threshold < 0.0f || settings.battery_disconnect_threshold > kBatteryVoltageAbsMaxUi) {
    settings = backup;
    error = "batteryDisconnectThreshold must be in range 0..60 V";
    return false;
  }

  if (settings.battery_low_threshold < 0.0f || settings.battery_low_threshold > kBatteryVoltageAbsMaxUi) {
    settings = backup;
    error = "batteryLowThreshold must be in range 0..60 V";
    return false;
  }

  if (settings.voltmeter_offset < -kVoltmeterOffsetAbsMaxUi || settings.voltmeter_offset > kVoltmeterOffsetAbsMaxUi) {
    settings = backup;
    error = "voltmeterOffset must be in range -25..25 V";
    return false;
  }
#endif

  if (!saveAppSettings()) {
    settings = backup;
    error = "failed to save settings";
    return false;
  }

  return true;
}

bool verifySettingsPin(const String& pin) {
  AppSettingsReadGuard guard;
  const AppSettings& settings = guard.settings();
  if (!settings.pin_protection_enabled) {
    return true;
  }
  String a = pin;
  a.trim();
  String b = settings.pin_code;
  b.trim();
  return a == b;
}
