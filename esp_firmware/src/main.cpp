#include "config.h"
#include "app_settings.h"

#include <Arduino.h>
#include <SPIFFS.h>

#if APP_MODE == APP_MODE_DAEMON
#include "ros_node.h"
#endif

#if ENABLE_LED_TASK
#include "led_control.h"
#endif

#include "battery_estimate.h"

#if ENABLE_SENSOR_TASK
#include "telemetry_log.h"
#include "voltmeter.h"
#endif

#if ENABLE_ESPNOW
#include "espnow_comm.h"
#endif

#if ENABLE_WEB_SERVER
#include "web_server.h"
#endif

#if ENABLE_WIFI || ENABLE_ESPNOW || ENABLE_WEB_SERVER
#include "wifi_config.h"
#endif

#if ENABLE_MICROROS
#include <rmw_microros/rmw_microros.h>
#endif

static TaskHandle_t ledTaskHandle = NULL;
static TaskHandle_t sensorTaskHandle = NULL;
static TaskHandle_t microRosTaskHandle = NULL;
static TaskHandle_t serviceTaskHandle = NULL;

static void createPinnedTask(
  TaskFunction_t fn,
  const char* name,
  const uint32_t stackBytes,
  const UBaseType_t priority,
  TaskHandle_t* outHandle) {
  BaseType_t result = xTaskCreatePinnedToCore(fn, name, stackBytes, NULL, priority, outHandle, TASK_CORE_ID);
  if (result != pdPASS) {
    DAEMON_LOGF("Failed to create task: %s\n", name);
  }
}

#if APP_MODE == APP_MODE_DAEMON
void microROSTask(void* pvParameters) {
  (void)pvParameters;
#if ENABLE_MICROROS
  if (!getAppSettings().runtime_microros_enabled) {
    vTaskDelete(NULL);
  }

  uint8_t consecutive_ping_failures = 0;

  while (true) {
    const AppSettings& settings = getAppSettings();
    switch (state) {
      case WAITING_AGENT:
        EXECUTE_EVERY_N_MS(settings.mros_ping_interval_ms, {
          state = (RMW_RET_OK == rmw_uros_ping_agent(settings.mros_timeout_ms, 1)) ? AGENT_AVAILABLE : WAITING_AGENT;
          if (state == AGENT_AVAILABLE) {
            consecutive_ping_failures = 0;
          }
        });
        break;
      case AGENT_AVAILABLE:
        state = (true == create_entities()) ? AGENT_CONNECTED : WAITING_AGENT;
        if (state == AGENT_CONNECTED) {
          consecutive_ping_failures = 0;
        } else {
          destroy_entities();
          state = WAITING_AGENT;
        }
        break;
      case AGENT_CONNECTED:
        EXECUTE_EVERY_N_MS(settings.mros_ping_interval_ms, {
          if (RMW_RET_OK == rmw_uros_ping_agent(settings.mros_timeout_ms, 1)) {
            consecutive_ping_failures = 0;
            state = AGENT_CONNECTED;
          } else {
            if (consecutive_ping_failures < 255) {
              consecutive_ping_failures++;
            }
            if (consecutive_ping_failures >= MROS_DISCONNECT_PING_FAILS) {
              state = AGENT_DISCONNECTED;
            }
          }
        });
        if (state == AGENT_CONNECTED) {
          rclc_executor_spin_some(&executor, RCL_MS_TO_NS(settings.ros_timer_ms));
        }
        break;
      case AGENT_DISCONNECTED:
        consecutive_ping_failures = 0;
        destroy_entities();
        state = WAITING_AGENT;
        break;
      default:
        break;
    }
    vTaskDelay(1);
  }
#else
  vTaskDelete(NULL);
#endif
}
#endif

void systemServiceTask(void* pvParameters) {
  (void)pvParameters;
  for (;;) {
#if ENABLE_WIFI
    handleWiFi();
#endif
#if ENABLE_WEB_SERVER
    handleWebServer();
#endif
#if ENABLE_ESPNOW
    handleESPNow();
#endif
#if ENABLE_POWER_CONTROL_POLL
    updatePowerControls();
#endif
    vTaskDelay(pdMS_TO_TICKS(TASK_SERVICE_INTERVAL_MS));
  }
}

void setup() {
  Serial.begin(115200);

  const bool spiffsMounted = SPIFFS.begin(false);
  if (!spiffsMounted) {
    DAEMON_LOGLN("SPIFFS Mount Failed");
  }
  initAppSettings(spiffsMounted);

  batteryEstimateInit();

#if ENABLE_SENSOR_TASK
  telemetryLogInit();
#endif

  // ================================================================
  //  NOTE: DO NOT CHANGE THE ORDER OF THE FOLLOWING INITIALIZATIONS
  // ================================================================

#if APP_MODE == APP_MODE_DAEMON
  initROS();
#endif

#if ENABLE_LED_TASK
  if (getAppSettings().runtime_led_enabled) {
    initLED();
    createPinnedTask(LEDTask, "LED Task", TASK_STACK_LED_BYTES, TASK_PRIO_LED, &ledTaskHandle);
  }
#endif

#if ENABLE_SENSOR_TASK
  if (getAppSettings().runtime_sensor_enabled) {
    initVoltmeter();
    createPinnedTask(voltmeterTask, "Sensor Task", TASK_STACK_SENSOR_BYTES, TASK_PRIO_SENSOR, &sensorTaskHandle);
  }
#endif

#if APP_MODE == APP_MODE_DAEMON && ENABLE_MICROROS
  if (getAppSettings().runtime_microros_enabled) {
    createPinnedTask(microROSTask, "microROS", TASK_STACK_MICROROS_BYTES, TASK_PRIO_MICROROS, &microRosTaskHandle);
  }
#endif

#if ENABLE_WIFI
  initWiFi();
#endif

#if ENABLE_ESPNOW
  if (getAppSettings().runtime_espnow_enabled) {
    initESPNow();
  }
#endif

#if ENABLE_WEB_SERVER
  initWebServer();
#endif

#if ENABLE_SYSTEM_SERVICE_TASK
  createPinnedTask(systemServiceTask, "Service Task", TASK_STACK_SERVICE_BYTES, TASK_PRIO_SERVICE, &serviceTaskHandle);
#endif
}

void loop() {}
