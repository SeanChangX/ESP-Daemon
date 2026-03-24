#include "voltmeter.h"
#include "app_settings.h"
#include "battery_estimate.h"
#include "telemetry_log.h"

#include "ros_node.h"
#include "led_control.h"

#include <ArduinoJson.h>
#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif

float Vbattf = 0.0;
uint32_t Vbatt = 0;
volatile int interruptCounter = 0;
hw_timer_t* _timer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

void initVoltmeter() {
  pinMode(getAppSettings().voltmeter_pin, INPUT);

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  _timer = timerBegin(1000000);  // 1 MHz timer base -> alarm value is in microseconds
  timerAttachInterrupt(_timer, &onTimer);
  timerAlarm(_timer, getAppSettings().timer_period_us, true, 0);
#else
  _timer = timerBegin(0, 80, true);
  timerAttachInterrupt(_timer, &onTimer, true);
  timerAlarmWrite(_timer, getAppSettings().timer_period_us, true);
  timerAlarmEnable(_timer);
#endif
}

void IRAM_ATTR onTimer() {
  portENTER_CRITICAL_ISR(&timerMux);
  interruptCounter++;
  portEXIT_CRITICAL_ISR(&timerMux);
}

void voltmeter() {
  // Debounce pack connection status so one noisy ADC sample does not
  // instantly flip UI between valid estimate and "--".
  static bool pack_connected_filtered = false;
  static uint8_t connected_streak = 0;
  static uint8_t disconnected_streak = 0;
  constexpr uint8_t kConnectConfirmSamples = 2;
  constexpr uint8_t kDisconnectConfirmSamples = 3;

  const AppSettings& settings = getAppSettings();
  const uint16_t sampleCount = settings.sliding_window_size > 0 ? settings.sliding_window_size : 1;
  const float dividerCalibration = (settings.voltage_divider_r2 > 0.0f)
    ? ((settings.voltage_divider_r1 + settings.voltage_divider_r2) / settings.voltage_divider_r2)
    : settings.voltmeter_calibration;

  Vbatt = 0;
  for (uint16_t i = 0; i < sampleCount; i++) {
    Vbatt += analogReadMilliVolts(settings.voltmeter_pin);
  }
  const float raw_pack_v = dividerCalibration * Vbatt / sampleCount / 1000.0f + settings.voltmeter_offset;
  const bool pack_connected_instant = raw_pack_v >= settings.battery_disconnect_threshold;

  if (pack_connected_instant) {
    connected_streak = connected_streak < 255 ? static_cast<uint8_t>(connected_streak + 1) : connected_streak;
    disconnected_streak = 0;
  } else {
    disconnected_streak = disconnected_streak < 255 ? static_cast<uint8_t>(disconnected_streak + 1) : disconnected_streak;
    connected_streak = 0;
  }

  if (!pack_connected_filtered) {
    if (connected_streak >= kConnectConfirmSamples) {
      pack_connected_filtered = true;
    }
  } else {
    if (disconnected_streak >= kDisconnectConfirmSamples) {
      pack_connected_filtered = false;
    }
  }

  if (!pack_connected_filtered) {
    Vbattf = 0.0f;
    batteryEstimateUpdate(0.0f, false);
    telemetryLogMaybePush(0.0f, false);
  } else {
    Vbattf = raw_pack_v;
    batteryEstimateUpdate(Vbattf, true);
    telemetryLogMaybePush(Vbattf, true);
  }
}

String getSensorReadings() {
  JsonDocument readings;
  const AppSettings& settings = getAppSettings();

  readings["sensor"]                    = String(Vbattf);
  readings["GND"]                       = 0;
  readings["controlGroup1Power"]        = getPowerControlState(POWER_CHANNEL_GROUP1);
  readings["controlGroup2Power"]        = getPowerControlState(POWER_CHANNEL_GROUP2);
  readings["controlGroup3Power"]        = getPowerControlState(POWER_CHANNEL_GROUP3);
  readings["controlGroup1Switch"]       = getPhysicalSwitchState(POWER_CHANNEL_GROUP1);
  readings["controlGroup2Switch"]       = getPhysicalSwitchState(POWER_CHANNEL_GROUP2);
  readings["controlGroup3Switch"]       = getPhysicalSwitchState(POWER_CHANNEL_GROUP3);
  readings["controlGroup1SwitchRaw"]    = getPhysicalSwitchRawLevel(POWER_CHANNEL_GROUP1);
  readings["controlGroup2SwitchRaw"]    = getPhysicalSwitchRawLevel(POWER_CHANNEL_GROUP2);
  readings["controlGroup3SwitchRaw"]    = getPhysicalSwitchRawLevel(POWER_CHANNEL_GROUP3);
  readings["controlGroup1Name"]         = settings.control_group1_name;
  readings["controlGroup2Name"]         = settings.control_group2_name;
  readings["controlGroup3Name"]         = settings.control_group3_name;
  readings["controlGroup1SwitchPin"]    = settings.control_group1_switch_pin;
  readings["controlGroup2SwitchPin"]    = settings.control_group2_switch_pin;
  readings["controlGroup3SwitchPin"]    = settings.control_group3_switch_pin;

  if      (Vbattf < settings.battery_disconnect_threshold) { readings["batteryStatus"] = "DISCONNECTED"; sensor_mode = BATT_DISCONNECTED; }
  else if (Vbattf < settings.battery_low_threshold)        { readings["batteryStatus"] = "LOW";          sensor_mode = BATT_LOW;          }
  else                                                     { readings["batteryStatus"] = "NORMAL";       sensor_mode = DEFAULT_MODE;      }

  batteryEstimateAppendJson(readings);

  switch (state) {
    case WAITING_AGENT:       readings["microROS"] = "WAITING_AGENT";       break;
    case AGENT_AVAILABLE:     readings["microROS"] = "AGENT_AVAILABLE";     break;
    case AGENT_CONNECTED:     readings["microROS"] = "AGENT_CONNECTED";     break;
    case AGENT_DISCONNECTED:  readings["microROS"] = "AGENT_DISCONNECTED";  break;
    default:                  readings["microROS"] = "UNKNOWN";             break;
  }

  String output;
  serializeJson(readings, output);
  return output;
}

void voltmeterTask(void* pvParameters) {
  for (;;) {
    voltmeter();
    if (interruptCounter > 0) {
      portENTER_CRITICAL(&timerMux);
      interruptCounter--;
      portEXIT_CRITICAL(&timerMux);
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}
