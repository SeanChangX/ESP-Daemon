#ifndef CONFIG_H
#define CONFIG_H

// WiFi and mDNS
#define HOSTNAME    "HERMES-ESP"
#define MDNS_NAME   "hermes-esp"

// micro-ROS
#define ROS_NODE_NAME       "esp_daemon"
#define ROS_DOMAIN_ID       0
#define ROS_TIMER_MS        500
#define MROS_TIMEOUT_MS     1000
#define MROS_PING_INTERVAL  1000

// Servo control
#define SERVO_D0_PIN        D0
#define SERVO_D1_PIN        D1  
#define SERVO_D2_PIN        D2
#define SERVO_D3_PIN        D3
#define SERVO_COUNT         4
#define SERVO_SEQUENCE_INTERVAL_MS  500
#define SERVO_SPEED_DELAY_MS        10
#define SERVO_STEP_SIZE             5

// RGB LED strip
#define LED_PIN             D10
#define LED_COUNT           26
#define LED_BRIGHTNESS      200
#define LED_OVR_DURATION    1000

// Voltmeter - Battery voltage measurement
// | Formula:
// |    Vbattf = (VOLTMETER_CALIBRATION * Vbatt / SLIDING_WINDOW_SIZE / 1000.0) + VOLTMETER_OFFSET;
// |    [ R1 = 1.5M ohm, R2 = 220k ohm ] VC = 7.81 OFFSET = 0.65
// |    [ R1 = 1.5M ohm, R2 = 200k ohm ] VC = 8.50 OFFSET = 0.65
// | Note:
// |    (A0 == D0) on Xiao ESP32C3
#define VOLTMETER_PIN           A0
#define VOLTMETER_CALIBRATION   8.5
#define VOLTMETER_OFFSET        0.1
#define SLIDING_WINDOW_SIZE     64
#define TIMER_PERIOD_US         1000000
// constexpr uint32_t TIMER_PERIOD_US = 1000000;

#endif
