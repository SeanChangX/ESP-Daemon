#include "voltmeter.h"
#include "app_settings.h"
#include "battery_estimate.h"
#include "telemetry_log.h"

#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif

float g_battery_voltage = 0.0f;
BatteryPackStatus g_battery_status = BATTERY_STATUS_DISCONNECTED;
volatile int interruptCounter = 0;
hw_timer_t* _timer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE batteryMux = portMUX_INITIALIZER_UNLOCKED;
constexpr int kMaxPendingSamples = 1;

void initVoltmeter() {
  AppSettingsReadGuard settingsGuard;
  const AppSettings& settings = settingsGuard.settings();
  pinMode(settings.voltmeter_pin, INPUT);
  portENTER_CRITICAL(&timerMux);
  interruptCounter = 0;
  portEXIT_CRITICAL(&timerMux);

  if (_timer != NULL) {
    timerEnd(_timer);
    _timer = NULL;
  }

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
  if (interruptCounter < kMaxPendingSamples) {
    interruptCounter++;
  }
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

  uint32_t sampleMvSum = 0;
  for (uint16_t i = 0; i < sampleCount; i++) {
    sampleMvSum += analogReadMilliVolts(voltmeterPin);
  }
  const float raw_pack_v = dividerCalibration * sampleMvSum / sampleCount / 1000.0f + voltmeterOffset;
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
    portENTER_CRITICAL(&batteryMux);
    g_battery_voltage = 0.0f;
    g_battery_status = BATTERY_STATUS_DISCONNECTED;
    portEXIT_CRITICAL(&batteryMux);
    batteryEstimateUpdate(0.0f, false);
    telemetryLogMaybePush(0.0f, false);
  } else {
    const BatteryPackStatus status = (raw_pack_v < batteryLowThreshold) ? BATTERY_STATUS_LOW : BATTERY_STATUS_NORMAL;
    portENTER_CRITICAL(&batteryMux);
    g_battery_voltage = raw_pack_v;
    g_battery_status = status;
    portEXIT_CRITICAL(&batteryMux);
    batteryEstimateUpdate(raw_pack_v, true);
    telemetryLogMaybePush(raw_pack_v, true);
  }
}

float getBatteryVoltage() {
  portENTER_CRITICAL(&batteryMux);
  const float voltage = g_battery_voltage;
  portEXIT_CRITICAL(&batteryMux);
  return voltage;
}

BatteryPackStatus getBatteryPackStatus() {
  portENTER_CRITICAL(&batteryMux);
  const BatteryPackStatus status = g_battery_status;
  portEXIT_CRITICAL(&batteryMux);
  return status;
}

void voltmeterTask(void* pvParameters) {
  (void)pvParameters;
  for (;;) {
    bool shouldSample = false;

    portENTER_CRITICAL(&timerMux);
    if (interruptCounter > 0) {
      interruptCounter--;
      shouldSample = true;
    }
    portEXIT_CRITICAL(&timerMux);

    if (shouldSample) {
      voltmeter();
      continue;
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
