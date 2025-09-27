#include "ros_node.h"
#include "config.h"

#include "led_control.h"

#include <Arduino.h>
#include <micro_ros_platformio.h>
#include <ESP32Servo.h>

// micro-ROS essential components
rcl_publisher_t                 counter_publisher;
rcl_subscription_t              landing_gear_subscriber;
rcl_subscription_t              battery_voltage_subscriber;
rcl_subscription_t              battery_current_subscriber;
std_msgs__msg__Int32            counter_msg;
std_msgs__msg__Bool             landing_gear_msg;
std_msgs__msg__Float32          battery_voltage_msg;
std_msgs__msg__Float32          battery_current_msg;

rclc_executor_t executor;
rcl_allocator_t allocator;
rclc_support_t support;
rcl_init_options_t init_options;
rcl_node_t node;
rcl_timer_t timer;

// Servo control for landing gear system
Servo servo_d0, servo_d1, servo_d2, servo_d3;
Servo* servos[SERVO_COUNT] = {&servo_d0, &servo_d1, &servo_d2, &servo_d3};
int retracted_angles[4] = {126, 124, 122, 131};
int extended_angles[4] = {43, 41, 39, 48};
bool is_gear_retracted = true;  // Start in takeoff state

// New servo state management
ServoState servo_states[SERVO_COUNT];
bool is_sequence_active = false;

// Battery data from mavros
float current_voltage = 0.0;
float current_current = 0.0;
float current_power = 0.0;

AgentState state = WAITING_AGENT;

#define RCCHECK(fn)        { rcl_ret_t temp_rc = fn; if ((temp_rc != RCL_RET_OK)) { error_loop(); } }
#define RCSOFTCHECK(fn)    { rcl_ret_t temp_rc = fn; if ((temp_rc != RCL_RET_OK)) {} }

void error_loop() { while (1) { delay(100); } }

void timer_callback(rcl_timer_t* timer, int64_t last_call_time) {
  RCLC_UNUSED(last_call_time);
  if (timer != NULL) {
    RCSOFTCHECK(rcl_publish(&counter_publisher, &counter_msg, NULL));
    counter_msg.data++;
  }
}



void battery_voltage_callback(const void* msgin) {
  const std_msgs__msg__Float32* incoming = (const std_msgs__msg__Float32*)msgin;
  
  // Simple voltage callback
  current_voltage = incoming->data;
  current_power = current_voltage * current_current;
}

void battery_current_callback(const void* msgin) {
  const std_msgs__msg__Float32* incoming = (const std_msgs__msg__Float32*)msgin;
  
  // Simple current callback
  current_current = incoming->data;
  current_power = current_voltage * current_current;
}

void landing_gear_callback(const void* msgin) {
  const std_msgs__msg__Bool* incoming = (const std_msgs__msg__Bool*)msgin;
  
  // Control landing gear servos with sequence
  if (incoming->data) { 
    // Retract landing gear (up position) - use sequence
    moveServoSequence(true);
    is_gear_retracted = true;
    mode = EME_ENABLE;  // Visual feedback on LED
  } else {
    // Extend landing gear (down position) - use sequence
    moveServoSequence(false);
    is_gear_retracted = false;
    mode = EME_DISABLE; // Visual feedback on LED
  }
  last_override_time = millis();
}

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
  rcl_subscription_fini(&landing_gear_subscriber, &node);
  rcl_subscription_fini(&battery_voltage_subscriber, &node);
  rcl_subscription_fini(&battery_current_subscriber, &node);
}

bool create_entities() {
  allocator = rcl_get_default_allocator();
  init_options = rcl_get_zero_initialized_init_options();
  rcl_init_options_init(&init_options, allocator);
  rcl_init_options_set_domain_id(&init_options, ROS_DOMAIN_ID);
  rclc_support_init_with_options(&support, 0, NULL, &init_options, &allocator);
  rclc_node_init_default(&node, ROS_NODE_NAME, "", &support);
  
  rclc_timer_init_default(&timer, &support, RCL_MS_TO_NS(ROS_TIMER_MS), timer_callback);

  rclc_publisher_init_default(
    &counter_publisher,
    &node,
    ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Int32),
    "/esp32_counter");


  // Initialize executor with 4 handles (3 subscriptions + 1 timer)
  unsigned int num_handles = 4;
  executor = rclc_executor_get_zero_initialized_executor();
  rclc_executor_init(&executor, &support.context, num_handles, &allocator);
  rclc_executor_add_timer(&executor, &timer);

  rclc_subscription_init_default(
    &landing_gear_subscriber,
    &node,
    ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Bool),
    "/landing_gear");
  rclc_executor_add_subscription(&executor, &landing_gear_subscriber, &landing_gear_msg, &landing_gear_callback, ON_NEW_DATA);


  rclc_subscription_init_default(
    &battery_voltage_subscriber,
    &node,
    ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Float32),
    "/battery_voltage");
  rclc_executor_add_subscription(&executor, &battery_voltage_subscriber, &battery_voltage_msg, &battery_voltage_callback, ON_NEW_DATA);

  rclc_subscription_init_default(
    &battery_current_subscriber,
    &node,
    ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Float32),
    "/battery_current");
  rclc_executor_add_subscription(&executor, &battery_current_subscriber, &battery_current_msg, &battery_current_callback, ON_NEW_DATA);
  
  return true;
}

void initROS() {
  set_microros_serial_transports(Serial);

  // Initialize servos on pins D0, D1, D2, D3
  servo_d0.attach(D0);
  servo_d1.attach(D1);
  servo_d2.attach(D2);
  servo_d3.attach(D3);
  
  // Set initial positions to retracted (landing gear up - takeoff state)
  servo_d0.write(retracted_angles[0]);
  servo_d1.write(retracted_angles[1]);
  servo_d2.write(retracted_angles[2]);
  servo_d3.write(retracted_angles[3]);
  
  // Initialize message data
  counter_msg.data = 0;
  landing_gear_msg.data = false;
  state = WAITING_AGENT;
  
  // Initialize servo states
  initServoStates();
}

// Initialize servo states for smooth movement control
void initServoStates() {
  for (int i = 0; i < SERVO_COUNT; i++) {
    servo_states[i].current_angle = retracted_angles[i];  // Start in retracted position (takeoff)
    servo_states[i].target_angle = retracted_angles[i];
    servo_states[i].is_moving = false;
    servo_states[i].last_move_time = 0;
    servo_states[i].sequence_start_time = 0;
  }
  is_sequence_active = false;
  
  // After initial setup, detach servos to save power (takeoff state)
  delay(1000);  // Give time for servos to reach position
  detachAllServos();
}

// Update servo movement for smooth control
void updateServoMovement() {
  unsigned long current_time = millis();
  
  // Handle sequence timing - start servos at their designated times
  if (is_sequence_active) {
    for (int i = 0; i < SERVO_COUNT; i++) {
      if (!servo_states[i].is_moving && 
          servo_states[i].current_angle != servo_states[i].target_angle &&
          current_time >= servo_states[i].sequence_start_time) {
        moveServoSmoothly(i, servo_states[i].target_angle);
      }
    }
  }
  
  // Update individual servo movements
  for (int i = 0; i < SERVO_COUNT; i++) {
    if (servo_states[i].is_moving) {
      // Check if enough time has passed for the next movement step
      if (current_time - servo_states[i].last_move_time >= SERVO_SPEED_DELAY_MS) {
        int diff = servo_states[i].target_angle - servo_states[i].current_angle;
        
        if (abs(diff) <= SERVO_STEP_SIZE) {
          // Close enough to target, set to target and stop moving
          servo_states[i].current_angle = servo_states[i].target_angle;
          servo_states[i].is_moving = false;
          servos[i]->write(servo_states[i].current_angle);
        } else {
          // Move one step towards target
          if (diff > 0) {
            servo_states[i].current_angle += SERVO_STEP_SIZE;
          } else {
            servo_states[i].current_angle -= SERVO_STEP_SIZE;
          }
          servos[i]->write(servo_states[i].current_angle);
          servo_states[i].last_move_time = current_time;
        }
      }
    }
  }
  
  // Check if sequence is complete
  if (is_sequence_active) {
    bool all_finished = true;
    for (int i = 0; i < SERVO_COUNT; i++) {
      if (servo_states[i].current_angle != servo_states[i].target_angle) {
        all_finished = false;
        break;
      }
    }
    if (all_finished) {
      is_sequence_active = false;
      
      // Detach servos if in takeoff state (retracted) to save power
      // Keep servos attached if in landing state (extended) to maintain position
      if (is_gear_retracted) {
        delay(500);  // Give a moment for servos to settle
        detachAllServos();
      }
    }
  }
}

// Start servo sequence movement with 1 second intervals
void moveServoSequence(bool retract) {
  if (is_sequence_active) return; // Don't start new sequence if one is already active
  
  // Attach all servos before starting movement
  attachAllServos();
  delay(100);  // Give servos time to attach
  
  is_sequence_active = true;
  unsigned long current_time = millis();
  
  // Set target angles and start times for each servo
  for (int i = 0; i < SERVO_COUNT; i++) {
    servo_states[i].target_angle = retract ? retracted_angles[i] : extended_angles[i];
    servo_states[i].sequence_start_time = current_time + (i * SERVO_SEQUENCE_INTERVAL_MS);
    servo_states[i].is_moving = false; // Reset moving state
  }
}

// Move individual servo smoothly to target angle
void moveServoSmoothly(int servo_index, int target_angle) {
  if (servo_index < 0 || servo_index >= SERVO_COUNT) return;
  
  servo_states[servo_index].target_angle = target_angle;
  servo_states[servo_index].is_moving = true;
  servo_states[servo_index].last_move_time = millis();
}

// Attach all servos for movement
void attachAllServos() {
  servo_d0.attach(D0);
  servo_d1.attach(D1);
  servo_d2.attach(D2);
  servo_d3.attach(D3);
}

// Detach all servos to save power
void detachAllServos() {
  servo_d0.detach();
  servo_d1.detach();
  servo_d2.detach();
  servo_d3.detach();
}
