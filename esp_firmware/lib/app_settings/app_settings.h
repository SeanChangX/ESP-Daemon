#ifndef APP_SETTINGS_H
#define APP_SETTINGS_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <array>
#include <vector>

struct EStopRouteConfig {
  String target_mac;
  uint8_t switch_pin;
  bool switch_active_high;
  bool switch_logic_inverted;
};

struct EmergencySourceConfig {
  std::array<uint8_t, 6> mac;
  bool control_group1_enabled;
  bool control_group2_enabled;
  bool control_group3_enabled;
};

struct AppSettings {
  String device_name;

  bool pin_protection_enabled;
  String pin_code;

  bool runtime_espnow_enabled;
  bool runtime_microros_enabled;
  bool runtime_led_enabled;
  bool runtime_sensor_enabled;

  String control_group1_name;
  String control_group2_name;
  String control_group3_name;

  uint8_t control_group1_switch_pin;
  uint8_t control_group2_switch_pin;
  uint8_t control_group3_switch_pin;

  uint8_t control_group1_power_pin;
  uint8_t control_group2_power_12v_pin;
  uint8_t control_group2_power_7v4_pin;
  uint8_t control_group3_power_pin;

  bool switch_active_high;
  bool power_active_high;

  uint8_t led_pin;
  uint16_t led_count;
  uint8_t led_brightness;
  uint32_t led_override_duration_ms;

  uint8_t voltmeter_pin;
  float voltage_divider_r1;
  float voltage_divider_r2;
  float voltmeter_calibration;
  float voltmeter_offset;
  uint16_t sliding_window_size;
  uint32_t timer_period_us;
  float battery_disconnect_threshold;
  float battery_low_threshold;

  String ros_node_name;
  int ros_domain_id;
  uint32_t ros_timer_ms;
  uint32_t mros_timeout_ms;
  uint32_t mros_ping_interval_ms;
  uint8_t espnow_channel;
  String estop_target_mac;
  uint8_t estop_switch_pin;
  bool estop_switch_active_high;
  bool estop_switch_logic_inverted;
  bool estop_wled_enabled;
  String estop_wled_base_url;
  uint16_t estop_wled_pressed_preset;
  uint16_t estop_wled_released_preset;
  bool estop_buzzer_enabled;
  uint8_t estop_buzzer_pin;
  std::vector<EStopRouteConfig> estop_routes;

  std::vector<EmergencySourceConfig> emergency_sources;

  /** Short-press target for status-page Control Panel FAB (path or http(s) URL). Default https://scx.tw/links. */
  String control_panel_url;
};

class AppSettingsReadGuard {
public:
  AppSettingsReadGuard();
  ~AppSettingsReadGuard();

  AppSettingsReadGuard(const AppSettingsReadGuard&) = delete;
  AppSettingsReadGuard& operator=(const AppSettingsReadGuard&) = delete;

  const AppSettings& settings() const;

private:
  bool locked_;
};

void initAppSettings(bool storage_available = true);
// Returns the live settings object. Prefer using AppSettingsReadGuard for
// thread-safe access from application modules.
const AppSettings& getAppSettings();

void appSettingsToJson(JsonDocument& doc, bool include_pin_code = false);
bool updateAppSettingsFromJson(const JsonObjectConst& json, String& error);
bool saveAppSettings();
bool resetAppSettingsToDefaults();
bool eraseAppSettingsFromNvs();

bool verifySettingsPin(const String& pin);

String toMacString(const std::array<uint8_t, 6>& mac);
bool parseMacString(const String& text, std::array<uint8_t, 6>& out);

#endif
