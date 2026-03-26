#include "voltmeter.h"
#include "app_settings.h"
#include "battery_estimate.h"
#include "telemetry_log.h"

#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif

float g_battery_voltage = 0.0f;
uint32_t Vbatt = 0;
volatile BatteryPackStatus g_battery_status = BATTERY_STATUS_DISCONNECTED;
volatile int interruptCounter = 0;
hw_timer_t* _timer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

void initVoltmeter() {
  AppSettingsReadGuard settingsGuard;
  const AppSettings& settings = settingsGuard.settings();
  pinMode(settings.voltmeter_pin, INPUT);

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  _timer = timerBegin(1000000);  // 1 MHz timer base -> alarm value is in microseconds
  timerAttachInterrupt(_timer, &onTimer);
  timerAlarm(_timer, settings.timer_period_us, true, 0);
#else
  _timer = timerBegin(0, 80, true);
  timerAttachInterrupt(_timer, &onTimer, true);
  timerAlarmWrite(_timer, settings.timer_period_us, true);
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
  static    bool    pack_connected_filtered   = false;
  static    uint8_t connected_streak          = 0;
  static    uint8_t disconnected_streak       = 0;
  constexpr uint8_t kConnectConfirmSamples    = 2;
  constexpr uint8_t kDisconnectConfirmSamples = 3;

  uint16_t  sampleCount                 = 1;
  uint8_t   voltmeterPin                = 0;
  float     dividerCalibration          = 1.0f;
  float     voltmeterOffset             = 0.0f;
  float     batteryDisconnectThreshold  = 0.0f;
  float     batteryLowThreshold         = 0.0f;
  {
    AppSettingsReadGuard settingsGuard;
    const AppSettings& settings = settingsGuard.settings();
    sampleCount = settings.sliding_window_size > 0 ? settings.sliding_window_size : 1;
    voltmeterPin = settings.voltmeter_pin;
    dividerCalibration = (settings.voltage_divider_r2 > 0.0f)
      ? ((settings.voltage_divider_r1 + settings.voltage_divider_r2) / settings.voltage_divider_r2)
      : settings.voltmeter_calibration;
    voltmeterOffset = settings.voltmeter_offset;
    batteryDisconnectThreshold = settings.battery_disconnect_threshold;
    batteryLowThreshold = settings.battery_low_threshold;
  }

  Vbatt = 0;
  for (uint16_t i = 0; i < sampleCount; i++) {
    Vbatt += analogReadMilliVolts(voltmeterPin);
  }
  const float raw_pack_v = dividerCalibration * Vbatt / sampleCount / 1000.0f + voltmeterOffset;
  const bool pack_connected_instant = raw_pack_v >= batteryDisconnectThreshold;

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
    g_battery_voltage = 0.0f;
    g_battery_status  = BATTERY_STATUS_DISCONNECTED;
    batteryEstimateUpdate(0.0f, false);
    telemetryLogMaybePush(0.0f, false);
  } else {
    g_battery_voltage = raw_pack_v;
    g_battery_status  = (g_battery_voltage < batteryLowThreshold) ? BATTERY_STATUS_LOW : BATTERY_STATUS_NORMAL;
    batteryEstimateUpdate(g_battery_voltage, true);
    telemetryLogMaybePush(g_battery_voltage, true);
  }
}

float getBatteryVoltage() {
  return g_battery_voltage;
}

BatteryPackStatus getBatteryPackStatus() {
  return g_battery_status;
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
