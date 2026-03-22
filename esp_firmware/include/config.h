#ifndef CONFIG_H
#define CONFIG_H

// ============================================================================
// Build Profile
// ============================================================================
// PROFILE_DEV: development profile with more debugging visibility.
// PROFILE_PROD: production profile with reduced debug noise.
#define PROFILE_DEV   1
#define PROFILE_PROD  2

#ifndef BUILD_PROFILE
#define BUILD_PROFILE PROFILE_DEV
#endif

#if BUILD_PROFILE == PROFILE_DEV
#define ENABLE_VERBOSE_LOG 1
#elif BUILD_PROFILE == PROFILE_PROD
#define ENABLE_VERBOSE_LOG 0
#else
#error "BUILD_PROFILE must be PROFILE_DEV or PROFILE_PROD"
#endif

// ============================================================================
// Application Mode
// ============================================================================
#define APP_MODE_DAEMON 1
#define APP_MODE_ESTOP  2

#ifndef APP_MODE
#define APP_MODE APP_MODE_DAEMON
#endif

#if APP_MODE != APP_MODE_DAEMON && APP_MODE != APP_MODE_ESTOP
#error "APP_MODE must be APP_MODE_DAEMON or APP_MODE_ESTOP"
#endif

// ============================================================================
// Compile-Time Feature Switches (0/1)
// ============================================================================
// This layer controls compile-time inclusion.
// If set to 0, that module is excluded from the firmware image.
// Runtime toggles in the web settings only work for modules compiled in.
#ifndef ENABLE_WIFI
#define ENABLE_WIFI                  1
#endif
#ifndef ENABLE_WEB_SERVER
#define ENABLE_WEB_SERVER            1
#endif
#ifndef ENABLE_ESPNOW
#define ENABLE_ESPNOW                1
#endif
#ifndef ENABLE_MICROROS
#if APP_MODE == APP_MODE_ESTOP
#define ENABLE_MICROROS              0
#else
#define ENABLE_MICROROS              1
#endif
#endif
#ifndef ENABLE_LED_TASK
#if APP_MODE == APP_MODE_ESTOP
#define ENABLE_LED_TASK              0
#else
#define ENABLE_LED_TASK              1
#endif
#endif
#ifndef ENABLE_SENSOR_TASK
#if APP_MODE == APP_MODE_ESTOP
#define ENABLE_SENSOR_TASK           0
#else
#define ENABLE_SENSOR_TASK           1
#endif
#endif
#ifndef ENABLE_SYSTEM_SERVICE_TASK
#define ENABLE_SYSTEM_SERVICE_TASK   1
#endif
#ifndef ENABLE_POWER_CONTROL_POLL
#if APP_MODE == APP_MODE_ESTOP
#define ENABLE_POWER_CONTROL_POLL    0
#else
#define ENABLE_POWER_CONTROL_POLL    1
#endif
#endif

// Dependency guards to prevent invalid feature combinations.
#if ENABLE_WEB_SERVER && !ENABLE_WIFI
#error "ENABLE_WEB_SERVER requires ENABLE_WIFI=1"
#endif
#if ENABLE_ESPNOW && !ENABLE_WIFI
#error "ENABLE_ESPNOW requires ENABLE_WIFI=1 in current implementation"
#endif

// ============================================================================
// Runtime Logging
// ============================================================================
// micro-ROS serial transport shares the same Serial link as debug print output.
// Keep serial logs OFF by default when micro-ROS is enabled to avoid XRCE frame
// corruption and random reconnect/disconnect behavior.
#ifndef ENABLE_SERIAL_LOG
#if ENABLE_MICROROS
#define ENABLE_SERIAL_LOG 0
#else
#define ENABLE_SERIAL_LOG 1
#endif
#endif

#if ENABLE_SERIAL_LOG
#define DAEMON_LOGF(...)  do { Serial.printf(__VA_ARGS__); } while (0)
#define DAEMON_LOGLN(...) do { Serial.println(__VA_ARGS__); } while (0)
#else
#define DAEMON_LOGF(...)  do {} while (0)
#define DAEMON_LOGLN(...) do {} while (0)
#endif

// ============================================================================
// RTOS Task Configuration
// ============================================================================
// ESP32-C3 is single-core, so tasks must run on core 0.
#ifndef TASK_CORE_ID
#define TASK_CORE_ID 0
#endif

// FreeRTOS priority: higher value means higher priority.
// Priority 1 is usually sufficient for stability-first behavior.
#ifndef TASK_PRIO_LED
#define TASK_PRIO_LED 1
#endif
#ifndef TASK_PRIO_SENSOR
#define TASK_PRIO_SENSOR 1
#endif
#ifndef TASK_PRIO_MICROROS
#define TASK_PRIO_MICROROS 1
#endif
#ifndef TASK_PRIO_SERVICE
#define TASK_PRIO_SERVICE 1
#endif

// Stack unit is bytes in ESP-IDF's xTaskCreatePinnedToCore() API.
// Example: 10000 bytes is approximately 9.8 KB.
#ifndef TASK_STACK_LED_BYTES
#define TASK_STACK_LED_BYTES 4096
#endif
#ifndef TASK_STACK_SENSOR_BYTES
#define TASK_STACK_SENSOR_BYTES 4096
#endif
#ifndef TASK_STACK_MICROROS_BYTES
#define TASK_STACK_MICROROS_BYTES 10000
#endif
#ifndef TASK_STACK_SERVICE_BYTES
#define TASK_STACK_SERVICE_BYTES 4096
#endif

// Service task polling interval in milliseconds.
#ifndef TASK_SERVICE_INTERVAL_MS
#define TASK_SERVICE_INTERVAL_MS 50
#endif

// Require several consecutive ping failures before declaring micro-ROS lost.
#ifndef MROS_DISCONNECT_PING_FAILS
#define MROS_DISCONNECT_PING_FAILS 3
#endif

// Basic compile-time safety guards to avoid unstable runtime settings.
#if TASK_STACK_LED_BYTES < 1024
#error "TASK_STACK_LED_BYTES too small (<1024)"
#endif
#if TASK_STACK_SENSOR_BYTES < 1024
#error "TASK_STACK_SENSOR_BYTES too small (<1024)"
#endif
#if TASK_STACK_MICROROS_BYTES < 2048
#error "TASK_STACK_MICROROS_BYTES too small (<2048)"
#endif
#if TASK_STACK_SERVICE_BYTES < 1024
#error "TASK_STACK_SERVICE_BYTES too small (<1024)"
#endif
#if TASK_SERVICE_INTERVAL_MS < 5
#error "TASK_SERVICE_INTERVAL_MS too small (<5ms)"
#endif
#if MROS_DISCONNECT_PING_FAILS < 1
#error "MROS_DISCONNECT_PING_FAILS must be >= 1"
#endif

// Runtime settings (GPIO/LED/ROS/hostname/voltmeter/ESP-NOW whitelist/PIN, etc.)
// are managed by Web Settings and persisted to /settings.json.

// Firmware version
#ifndef ESP_DAEMON_FW_VERSION
#define ESP_DAEMON_FW_VERSION "2.1.3"
#endif

#endif
