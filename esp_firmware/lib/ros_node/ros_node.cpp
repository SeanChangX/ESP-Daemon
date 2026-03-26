#include "ros_node.h"
#include "app_settings.h"

#include "led_control.h"
#include "voltmeter.h"

#include <Arduino.h>
#if ENABLE_MICROROS
#include <micro_ros_platformio.h>
#endif

#if ENABLE_MICROROS
// micro-ROS essential components
rcl_publisher_t                        counter_publisher;
rcl_publisher_t                battery_voltage_publisher;
rcl_subscription_t       control_group1_enable_subscriber;
rcl_subscription_t       control_group2_enable_subscriber;
rcl_subscription_t       control_group3_enable_subscriber;
std_msgs__msg__Int32                   counter_msg;
std_msgs__msg__Float32         battery_voltage_msg;
std_msgs__msg__Bool      control_group1_enable_msg;
std_msgs__msg__Bool      control_group2_enable_msg;
std_msgs__msg__Bool      control_group3_enable_msg;

rclc_executor_t          executor;
rcl_allocator_t          allocator;
rclc_support_t           support;
rcl_init_options_t       init_options;
rcl_node_t               node;
rcl_timer_t              timer;
#endif

#if ENABLE_MICROROS
AgentState state = WAITING_AGENT;
#else
AgentState state = AGENT_DISCONNECTED;
#endif

// Remote (web / ROS / HTTP) commands use explicit force-on / force-off so OFF overrides physical switches.
// Physical switch edge returns that rail to FollowPhysical (local operator regains sole control).
enum class RemoteRailMode : uint8_t { FollowPhysical = 0, RemoteForceOn = 1, RemoteForceOff = 2 };

static volatile RemoteRailMode control_group1_remote_mode = RemoteRailMode::FollowPhysical;
static volatile RemoteRailMode control_group2_remote_mode = RemoteRailMode::FollowPhysical;
static volatile RemoteRailMode control_group3_remote_mode = RemoteRailMode::FollowPhysical;
static volatile bool control_group1_remote_emergency_latched = false;

static volatile bool control_group1_power_enabled = false;
static volatile bool control_group2_power_enabled = false;
static volatile bool control_group3_power_enabled = false;

#if ENABLE_MICROROS
#define RCCHECK(fn)        { rcl_ret_t temp_rc = fn; if ((temp_rc != RCL_RET_OK)) { error_loop(); } }
#define RCSOFTCHECK(fn)    { rcl_ret_t temp_rc = fn; if ((temp_rc != RCL_RET_OK)) {} }

void error_loop() { while (1) { delay(100); } }
#endif

static bool isSwitchEnabled(uint8_t pin) {
  AppSettingsReadGuard settingsGuard;
  const bool activeHigh = settingsGuard.settings().switch_active_high;
  return digitalRead(pin) == (activeHigh ? HIGH : LOW);
}

static uint8_t getSwitchPinForChannel(PowerControlChannel channel) {
  AppSettingsReadGuard settingsGuard;
  const AppSettings& settings = settingsGuard.settings();
  switch (channel) {
    case POWER_CHANNEL_GROUP1:
      return settings.control_group1_switch_pin;
    case POWER_CHANNEL_GROUP2:
      return settings.control_group2_switch_pin;
    case POWER_CHANNEL_GROUP3:
      return settings.control_group3_switch_pin;
    default:
      return settings.control_group1_switch_pin;
  }
}

static void setOutputPower(uint8_t pin, bool enabled, bool powerActiveHigh) {
  digitalWrite(pin, enabled ? (powerActiveHigh ? HIGH : LOW) : (powerActiveHigh ? LOW : HIGH));
}

static void applyPowerOutputs(bool group1_enabled, bool group2_enabled, bool group3_enabled) {
  AppSettingsReadGuard settingsGuard;
  const AppSettings& settings = settingsGuard.settings();
  setOutputPower(settings.control_group1_power_pin,           group1_enabled, settings.power_active_high);
  setOutputPower(settings.control_group2_power_12v_pin,       group2_enabled, settings.power_active_high);
  setOutputPower(settings.control_group2_power_7v4_pin,       group2_enabled, settings.power_active_high);
  setOutputPower(settings.control_group3_power_pin,           group3_enabled, settings.power_active_high);
}

static void refreshPowerControlState() {
  static bool has_prev_switch_sample = false;
  static bool prev_group1_switch     = false;
  static bool prev_group2_switch     = false;
  static bool prev_group3_switch     = false;

  AppSettingsReadGuard settingsGuard;
  const AppSettings& settings = settingsGuard.settings();
  const bool new_group1_switch = isSwitchEnabled(settings.control_group1_switch_pin);
  const bool new_group2_switch = isSwitchEnabled(settings.control_group2_switch_pin);
  const bool new_group3_switch = isSwitchEnabled(settings.control_group3_switch_pin);

  const bool group1_switch_changed = has_prev_switch_sample && (new_group1_switch != prev_group1_switch);
  const bool group2_switch_changed = has_prev_switch_sample && (new_group2_switch != prev_group2_switch);
  const bool group3_switch_changed = has_prev_switch_sample && (new_group3_switch != prev_group3_switch);

  // Physical switch change: return to local-only control and clear group1 emergency latch.
  if (group1_switch_changed) {
    control_group1_remote_mode = RemoteRailMode::FollowPhysical;
    control_group1_remote_emergency_latched = false;
  }
  if (group2_switch_changed) {
    control_group2_remote_mode = RemoteRailMode::FollowPhysical;
  }
  if (group3_switch_changed) {
    control_group3_remote_mode = RemoteRailMode::FollowPhysical;
  }

  bool new_group1_power = false;
  if (control_group1_remote_emergency_latched) {
    new_group1_power = false;
  } else {
    switch (control_group1_remote_mode) {
      case RemoteRailMode::RemoteForceOff:
        new_group1_power = false;
        break;
      case RemoteRailMode::RemoteForceOn:
        new_group1_power = true;
        break;
      case RemoteRailMode::FollowPhysical:
      default:
        new_group1_power = new_group1_switch;
        break;
    }
  }

  bool new_group2_power = false;
  switch (control_group2_remote_mode) {
    case RemoteRailMode::RemoteForceOff:
      new_group2_power = false;
      break;
    case RemoteRailMode::RemoteForceOn:
      new_group2_power = true;
      break;
    case RemoteRailMode::FollowPhysical:
    default:
      new_group2_power = new_group2_switch;
      break;
  }

  bool new_group3_power = false;
  switch (control_group3_remote_mode) {
    case RemoteRailMode::RemoteForceOff:
      new_group3_power = false;
      break;
    case RemoteRailMode::RemoteForceOn:
      new_group3_power = true;
      break;
    case RemoteRailMode::FollowPhysical:
    default:
      new_group3_power = new_group3_switch;
      break;
  }

  const bool group1_changed = (new_group1_power != control_group1_power_enabled);
  const bool group2_changed = (new_group2_power != control_group2_power_enabled);
  const bool group3_changed = (new_group3_power != control_group3_power_enabled);

  if (group1_changed || group2_changed || group3_changed) {
    applyPowerOutputs(new_group1_power, new_group2_power, new_group3_power);
  }

  prev_group1_switch = new_group1_switch;
  prev_group2_switch = new_group2_switch;
  prev_group3_switch = new_group3_switch;
  has_prev_switch_sample = true;

  if (group1_changed) {
    mode = new_group1_power ? EME_ENABLE : EME_DISABLE;
    last_override_time = millis();
  }

  control_group1_power_enabled = new_group1_power;
  control_group2_power_enabled = new_group2_power;
  control_group3_power_enabled = new_group3_power;
}

#if ENABLE_MICROROS
void timer_callback(rcl_timer_t* timer, int64_t last_call_time) {
  RCLC_UNUSED(last_call_time);
  if (timer != NULL) {
    RCSOFTCHECK(rcl_publish(&counter_publisher, &counter_msg, NULL));
    RCSOFTCHECK(rcl_publish(&battery_voltage_publisher, &battery_voltage_msg, NULL));
    counter_msg.data++;
    battery_voltage_msg.data = getBatteryVoltage();
  }
}

void control_group1_enable_callback(const void* msgin) {
  const std_msgs__msg__Bool* incoming = (const std_msgs__msg__Bool*)msgin;
  setPowerControlOverride(POWER_CHANNEL_GROUP1, incoming->data);
}

void control_group2_enable_callback(const void* msgin) {
  const std_msgs__msg__Bool* incoming = (const std_msgs__msg__Bool*)msgin;
  setPowerControlOverride(POWER_CHANNEL_GROUP2, incoming->data);
}

void control_group3_enable_callback(const void* msgin) {
  const std_msgs__msg__Bool* incoming = (const std_msgs__msg__Bool*)msgin;
  setPowerControlOverride(POWER_CHANNEL_GROUP3, incoming->data);
}
#endif

void updatePowerControls() {
  refreshPowerControlState();
}

void setPowerControlOverride(PowerControlChannel channel, bool enabled) {
  switch (channel) {
    case POWER_CHANNEL_GROUP1:
      if (enabled) {
        control_group1_remote_emergency_latched = false;
        control_group1_remote_mode = RemoteRailMode::RemoteForceOn;
      } else {
        control_group1_remote_mode = RemoteRailMode::RemoteForceOff;
      }
      break;
    case POWER_CHANNEL_GROUP2:
      control_group2_remote_mode = enabled ? RemoteRailMode::RemoteForceOn : RemoteRailMode::RemoteForceOff;
      break;
    case POWER_CHANNEL_GROUP3:
      control_group3_remote_mode = enabled ? RemoteRailMode::RemoteForceOn : RemoteRailMode::RemoteForceOff;
      break;
    default:
      return;
  }

  refreshPowerControlState();
}

void triggerRemoteEmergencyStop(bool target_group1, bool target_group2, bool target_group3) {
  if (!target_group1 && !target_group2 && !target_group3) {
    return;
  }

  // ESP-NOW emergency: selected groups are forced off.
  // Group1 keeps dedicated emergency latch semantics.
  if (target_group1) {
    control_group1_remote_emergency_latched = true;
    control_group1_remote_mode = RemoteRailMode::FollowPhysical;
  }
  if (target_group2) {
    control_group2_remote_mode = RemoteRailMode::RemoteForceOff;
  }
  if (target_group3) {
    control_group3_remote_mode = RemoteRailMode::RemoteForceOff;
  }
  refreshPowerControlState();
}

bool getPowerControlState(PowerControlChannel channel) {
  switch (channel) {
    case POWER_CHANNEL_GROUP1:
      return control_group1_power_enabled;
    case POWER_CHANNEL_GROUP2:
      return control_group2_power_enabled;
    case POWER_CHANNEL_GROUP3:
      return control_group3_power_enabled;
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
  rcl_subscription_fini(&control_group1_enable_subscriber, &node);
  rcl_subscription_fini(&control_group2_enable_subscriber, &node);
  rcl_subscription_fini(&control_group3_enable_subscriber, &node);
}

bool create_entities() {
  AppSettingsReadGuard settingsGuard;
  const AppSettings& settings = settingsGuard.settings();
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
    &control_group1_enable_subscriber,
    &node,
    ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Bool),
    "/robot_status/control_group1_enable");
  rclc_executor_add_subscription(&executor, &control_group1_enable_subscriber, &control_group1_enable_msg, &control_group1_enable_callback, ON_NEW_DATA);

  rclc_subscription_init_default(
    &control_group2_enable_subscriber,
    &node,
    ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Bool),
    "/robot_status/control_group2_enable");
  rclc_executor_add_subscription(&executor, &control_group2_enable_subscriber, &control_group2_enable_msg, &control_group2_enable_callback, ON_NEW_DATA);

  rclc_subscription_init_default(
    &control_group3_enable_subscriber,
    &node,
    ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Bool),
    "/robot_status/control_group3_enable");
  rclc_executor_add_subscription(&executor, &control_group3_enable_subscriber, &control_group3_enable_msg, &control_group3_enable_callback, ON_NEW_DATA);
  
  return true;
}
#endif

void initROS() {
#if ENABLE_MICROROS
  set_microros_serial_transports(Serial);
#endif

  AppSettingsReadGuard settingsGuard;
  const AppSettings& settings = settingsGuard.settings();
  const uint8_t inputMode = settings.switch_active_high ? INPUT : INPUT_PULLUP;

  pinMode(settings.control_group1_switch_pin, inputMode);
  pinMode(settings.control_group2_switch_pin, inputMode);
  pinMode(settings.control_group3_switch_pin, inputMode);

  pinMode(settings.control_group1_power_pin,     OUTPUT);
  pinMode(settings.control_group2_power_12v_pin, OUTPUT);
  pinMode(settings.control_group2_power_7v4_pin, OUTPUT);
  pinMode(settings.control_group3_power_pin,     OUTPUT);

  setOutputPower(settings.control_group1_power_pin,     false, settings.power_active_high);
  setOutputPower(settings.control_group2_power_12v_pin, false, settings.power_active_high);
  setOutputPower(settings.control_group2_power_7v4_pin, false, settings.power_active_high);
  setOutputPower(settings.control_group3_power_pin,     false, settings.power_active_high);
  
#if ENABLE_MICROROS
  counter_msg.data = 0;
  battery_voltage_msg.data = 0.0;
  control_group1_enable_msg.data = false;
  control_group2_enable_msg.data = false;
  control_group3_enable_msg.data = false;
  state = settings.runtime_microros_enabled ? WAITING_AGENT : AGENT_DISCONNECTED;
#else
  state = AGENT_DISCONNECTED;
#endif

  updatePowerControls();
}
