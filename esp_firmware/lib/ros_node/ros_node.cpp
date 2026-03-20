#include "ros_node.h"
#include "app_settings.h"

#include "led_control.h"

#include <Arduino.h>
#if ENABLE_MICROROS
#include <micro_ros_platformio.h>
#endif

#if ENABLE_MICROROS
// micro-ROS essential components
rcl_publisher_t                  counter_publisher;
rcl_publisher_t          battery_voltage_publisher;
rcl_subscription_t        chassis_enable_subscriber;
rcl_subscription_t        mission_enable_subscriber;
rcl_subscription_t   neg_pressure_enable_subscriber;
std_msgs__msg__Int32             counter_msg;
std_msgs__msg__Float32   battery_voltage_msg;
std_msgs__msg__Bool       chassis_enable_msg;
std_msgs__msg__Bool       mission_enable_msg;
std_msgs__msg__Bool  neg_pressure_enable_msg;

rclc_executor_t executor;
rcl_allocator_t allocator;
rclc_support_t support;
rcl_init_options_t init_options;
rcl_node_t node;
rcl_timer_t timer;
#endif

extern float Vbattf;
#if ENABLE_MICROROS
AgentState state = WAITING_AGENT;
#else
AgentState state = AGENT_DISCONNECTED;
#endif

// Remote (web / ROS / HTTP) commands use explicit force-on / force-off so OFF overrides physical switches.
// Physical switch edge returns that rail to FollowPhysical (local operator regains sole control).
enum class RemoteRailMode : uint8_t { FollowPhysical = 0, RemoteForceOn = 1, RemoteForceOff = 2 };

static volatile RemoteRailMode chassis_remote_mode = RemoteRailMode::FollowPhysical;
static volatile RemoteRailMode mission_remote_mode = RemoteRailMode::FollowPhysical;
static volatile RemoteRailMode neg_pressure_remote_mode = RemoteRailMode::FollowPhysical;
static volatile bool chassis_remote_emergency_latched = false;

static volatile bool chassis_power_enabled = false;
static volatile bool mission_power_enabled = false;
static volatile bool neg_pressure_power_enabled = false;

#if ENABLE_MICROROS
#define RCCHECK(fn)        { rcl_ret_t temp_rc = fn; if ((temp_rc != RCL_RET_OK)) { error_loop(); } }
#define RCSOFTCHECK(fn)    { rcl_ret_t temp_rc = fn; if ((temp_rc != RCL_RET_OK)) {} }

void error_loop() { while (1) { delay(100); } }
#endif

static bool isSwitchEnabled(uint8_t pin) {
  const bool activeHigh = getAppSettings().switch_active_high;
  return digitalRead(pin) == (activeHigh ? HIGH : LOW);
}

static uint8_t getSwitchPinForChannel(PowerControlChannel channel) {
  const AppSettings& settings = getAppSettings();
  switch (channel) {
    case POWER_CHANNEL_CHASSIS:
      return settings.chassis_switch_pin;
    case POWER_CHANNEL_MISSION:
      return settings.mission_switch_pin;
    case POWER_CHANNEL_NEG_PRESSURE:
      return settings.neg_pressure_switch_pin;
    default:
      return settings.chassis_switch_pin;
  }
}

static void setOutputPower(uint8_t pin, bool enabled) {
  const bool powerActiveHigh = getAppSettings().power_active_high;
  digitalWrite(pin, enabled ? (powerActiveHigh ? HIGH : LOW) : (powerActiveHigh ? LOW : HIGH));
}

static void applyPowerOutputs(bool chassis_enabled, bool mission_enabled, bool neg_pressure_enabled) {
  const AppSettings& settings = getAppSettings();
  setOutputPower(settings.chassis_power_pin,           chassis_enabled);
  setOutputPower(settings.mission_power_12v_pin,       mission_enabled);
  setOutputPower(settings.mission_power_7v4_pin,       mission_enabled);
  setOutputPower(settings.neg_pressure_power_pin, neg_pressure_enabled);
}

static void refreshPowerControlState() {
  static bool has_prev_switch_sample = false;
  static bool prev_chassis_switch = false;
  static bool prev_mission_switch = false;
  static bool prev_neg_pressure_switch = false;

  const AppSettings& settings = getAppSettings();
  const bool new_chassis_switch = isSwitchEnabled(settings.chassis_switch_pin);
  const bool new_mission_switch = isSwitchEnabled(settings.mission_switch_pin);
  const bool new_neg_pressure_switch = isSwitchEnabled(settings.neg_pressure_switch_pin);

  const bool chassis_switch_changed = has_prev_switch_sample && (new_chassis_switch != prev_chassis_switch);
  const bool mission_switch_changed = has_prev_switch_sample && (new_mission_switch != prev_mission_switch);
  const bool neg_pressure_switch_changed = has_prev_switch_sample && (new_neg_pressure_switch != prev_neg_pressure_switch);

  // Physical switch change: return to local-only control and clear chassis emergency latch.
  if (chassis_switch_changed) {
    chassis_remote_mode = RemoteRailMode::FollowPhysical;
    chassis_remote_emergency_latched = false;
  }
  if (mission_switch_changed) {
    mission_remote_mode = RemoteRailMode::FollowPhysical;
  }
  if (neg_pressure_switch_changed) {
    neg_pressure_remote_mode = RemoteRailMode::FollowPhysical;
  }

  bool new_chassis_power = false;
  if (chassis_remote_emergency_latched) {
    new_chassis_power = false;
  } else {
    switch (chassis_remote_mode) {
      case RemoteRailMode::RemoteForceOff:
        new_chassis_power = false;
        break;
      case RemoteRailMode::RemoteForceOn:
        new_chassis_power = true;
        break;
      case RemoteRailMode::FollowPhysical:
      default:
        new_chassis_power = new_chassis_switch;
        break;
    }
  }

  bool new_mission_power = false;
  switch (mission_remote_mode) {
    case RemoteRailMode::RemoteForceOff:
      new_mission_power = false;
      break;
    case RemoteRailMode::RemoteForceOn:
      new_mission_power = true;
      break;
    case RemoteRailMode::FollowPhysical:
    default:
      new_mission_power = new_mission_switch;
      break;
  }

  bool new_neg_pressure_power = false;
  switch (neg_pressure_remote_mode) {
    case RemoteRailMode::RemoteForceOff:
      new_neg_pressure_power = false;
      break;
    case RemoteRailMode::RemoteForceOn:
      new_neg_pressure_power = true;
      break;
    case RemoteRailMode::FollowPhysical:
    default:
      new_neg_pressure_power = new_neg_pressure_switch;
      break;
  }

  const bool chassis_changed = (new_chassis_power != chassis_power_enabled);
  const bool mission_changed = (new_mission_power != mission_power_enabled);
  const bool neg_pressure_changed = (new_neg_pressure_power != neg_pressure_power_enabled);

  if (chassis_changed || mission_changed || neg_pressure_changed) {
    applyPowerOutputs(new_chassis_power, new_mission_power, new_neg_pressure_power);
  }

  prev_chassis_switch = new_chassis_switch;
  prev_mission_switch = new_mission_switch;
  prev_neg_pressure_switch = new_neg_pressure_switch;
  has_prev_switch_sample = true;

  if (chassis_changed) {
    mode = new_chassis_power ? EME_ENABLE : EME_DISABLE;
    last_override_time = millis();
  }

  chassis_power_enabled = new_chassis_power;
  mission_power_enabled = new_mission_power;
  neg_pressure_power_enabled = new_neg_pressure_power;
}

#if ENABLE_MICROROS
void timer_callback(rcl_timer_t* timer, int64_t last_call_time) {
  RCLC_UNUSED(last_call_time);
  if (timer != NULL) {
    RCSOFTCHECK(rcl_publish(&counter_publisher, &counter_msg, NULL));
    RCSOFTCHECK(rcl_publish(&battery_voltage_publisher, &battery_voltage_msg, NULL));
    counter_msg.data++;
    battery_voltage_msg.data = Vbattf;
  }
}

void chassis_enable_callback(const void* msgin) {
  const std_msgs__msg__Bool* incoming = (const std_msgs__msg__Bool*)msgin;
  setPowerControlOverride(POWER_CHANNEL_CHASSIS, incoming->data);
}

void mission_enable_callback(const void* msgin) {
  const std_msgs__msg__Bool* incoming = (const std_msgs__msg__Bool*)msgin;
  setPowerControlOverride(POWER_CHANNEL_MISSION, incoming->data);
}

void neg_pressure_enable_callback(const void* msgin) {
  const std_msgs__msg__Bool* incoming = (const std_msgs__msg__Bool*)msgin;
  setPowerControlOverride(POWER_CHANNEL_NEG_PRESSURE, incoming->data);
}
#endif

void updatePowerControls() {
  refreshPowerControlState();
}

void setPowerControlOverride(PowerControlChannel channel, bool enabled) {
  switch (channel) {
    case POWER_CHANNEL_CHASSIS:
      if (enabled) {
        chassis_remote_emergency_latched = false;
        chassis_remote_mode = RemoteRailMode::RemoteForceOn;
      } else {
        chassis_remote_mode = RemoteRailMode::RemoteForceOff;
      }
      break;
    case POWER_CHANNEL_MISSION:
      mission_remote_mode = enabled ? RemoteRailMode::RemoteForceOn : RemoteRailMode::RemoteForceOff;
      break;
    case POWER_CHANNEL_NEG_PRESSURE:
      neg_pressure_remote_mode = enabled ? RemoteRailMode::RemoteForceOn : RemoteRailMode::RemoteForceOff;
      break;
    default:
      return;
  }

  refreshPowerControlState();
}

void triggerRemoteEmergencyStop() {
  // ESP-NOW stop: latch chassis off until web enable or physical chassis switch toggle.
  chassis_remote_emergency_latched = true;
  chassis_remote_mode = RemoteRailMode::FollowPhysical;
  refreshPowerControlState();
}

bool getPowerControlState(PowerControlChannel channel) {
  switch (channel) {
    case POWER_CHANNEL_CHASSIS:
      return chassis_power_enabled;
    case POWER_CHANNEL_MISSION:
      return mission_power_enabled;
    case POWER_CHANNEL_NEG_PRESSURE:
      return neg_pressure_power_enabled;
    default:
      return false;
  }
}

bool getPhysicalSwitchState(PowerControlChannel channel) {
  return isSwitchEnabled(getSwitchPinForChannel(channel));
}

int getPhysicalSwitchRawLevel(PowerControlChannel channel) {
  return digitalRead(getSwitchPinForChannel(channel));
}

#if ENABLE_MICROROS
// Free the resources allocated by micro-ROS
void destroy_entities() {
  rmw_context_t * rmw_context = rcl_context_get_rmw_context(&support.context);
  (void) rmw_uros_set_context_entity_destroy_session_timeout(rmw_context, 0);

  rcl_timer_fini(&timer);
  rclc_executor_fini(&executor);
  rcl_init_options_fini(&init_options);
  rcl_node_fini(&node);
  rclc_support_fini(&support);

  rcl_publisher_fini(&counter_publisher, &node);
  rcl_publisher_fini(&battery_voltage_publisher, &node);
  rcl_subscription_fini(&chassis_enable_subscriber, &node);
  rcl_subscription_fini(&mission_enable_subscriber, &node);
  rcl_subscription_fini(&neg_pressure_enable_subscriber, &node);
}

bool create_entities() {
  const AppSettings& settings = getAppSettings();
  allocator = rcl_get_default_allocator();
  init_options = rcl_get_zero_initialized_init_options();
  rcl_init_options_init(&init_options, allocator);
  rcl_init_options_set_domain_id(&init_options, settings.ros_domain_id);
  rclc_support_init_with_options(&support, 0, NULL, &init_options, &allocator);
  rclc_node_init_default(&node, settings.ros_node_name.c_str(), "", &support);
  
  rclc_timer_init_default(&timer, &support, RCL_MS_TO_NS(settings.ros_timer_ms), timer_callback);

  rclc_publisher_init_default(
    &counter_publisher,
    &node,
    ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Int32),
    "/esp32_counter");

  rclc_publisher_init_default(
    &battery_voltage_publisher,
    &node,
    ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Float32),
    "/robot_status/battery_voltage");


  // Initialize executor with 4 handles (3 subscriptions + 1 timer)
  unsigned int num_handles = 4;
  executor = rclc_executor_get_zero_initialized_executor();
  rclc_executor_init(&executor, &support.context, num_handles, &allocator);
  rclc_executor_add_timer(&executor, &timer);

  rclc_subscription_init_default(
    &chassis_enable_subscriber,
    &node,
    ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Bool),
    "/robot_status/chassis_enable");
  rclc_executor_add_subscription(&executor, &chassis_enable_subscriber, &chassis_enable_msg, &chassis_enable_callback, ON_NEW_DATA);

  rclc_subscription_init_default(
    &mission_enable_subscriber,
    &node,
    ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Bool),
    "/robot_status/mission_enable");
  rclc_executor_add_subscription(&executor, &mission_enable_subscriber, &mission_enable_msg, &mission_enable_callback, ON_NEW_DATA);

  rclc_subscription_init_default(
    &neg_pressure_enable_subscriber,
    &node,
    ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Bool),
    "/robot_status/neg_pressure_enable");
  rclc_executor_add_subscription(&executor, &neg_pressure_enable_subscriber, &neg_pressure_enable_msg, &neg_pressure_enable_callback, ON_NEW_DATA);
  
  return true;
}
#endif

void initROS() {
#if ENABLE_MICROROS
  set_microros_serial_transports(Serial);
#endif

  const AppSettings& settings = getAppSettings();
  const uint8_t inputMode = settings.switch_active_high ? INPUT : INPUT_PULLUP;

  pinMode(settings.chassis_switch_pin, inputMode);
  pinMode(settings.mission_switch_pin, inputMode);
  pinMode(settings.neg_pressure_switch_pin, inputMode);

  pinMode(settings.chassis_power_pin, OUTPUT);
  pinMode(settings.mission_power_12v_pin, OUTPUT);
  pinMode(settings.mission_power_7v4_pin, OUTPUT);
  pinMode(settings.neg_pressure_power_pin, OUTPUT);

  setOutputPower(settings.chassis_power_pin, false);
  setOutputPower(settings.mission_power_12v_pin, false);
  setOutputPower(settings.mission_power_7v4_pin, false);
  setOutputPower(settings.neg_pressure_power_pin, false);
  
#if ENABLE_MICROROS
  counter_msg.data = 0;
  battery_voltage_msg.data = 0.0;
  chassis_enable_msg.data = false;
  mission_enable_msg.data = false;
  neg_pressure_enable_msg.data = false;
  state = getAppSettings().runtime_microros_enabled ? WAITING_AGENT : AGENT_DISCONNECTED;
#else
  state = AGENT_DISCONNECTED;
#endif

  updatePowerControls();
}
