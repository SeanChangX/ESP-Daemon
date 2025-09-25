#ifndef ROS_NODE_H
#define ROS_NODE_H

#include <rcl/rcl.h>
#include <rclc/rclc.h>
#include <rclc/executor.h>
#include <std_msgs/msg/bool.h>
#include <std_msgs/msg/int32.h>
#include <sensor_msgs/msg/battery_state.h>
#include "config.h"

#define EXECUTE_EVERY_N_MS(MS, X)  do { \
    static volatile int64_t init = -1; \
    if (init == -1) { init = uxr_millis();} \
    if (uxr_millis() - init > MS) { X; init = uxr_millis();} \
} while (0)

enum AgentState {
  WAITING_AGENT,
  AGENT_AVAILABLE,
  AGENT_CONNECTED,
  AGENT_DISCONNECTED
};

// Servo state structure for smooth movement and sequencing
struct ServoState {
  int current_angle;
  int target_angle;
  bool is_moving;
  unsigned long last_move_time;
  unsigned long sequence_start_time;
};

extern AgentState state;

void initROS();
bool create_entities();
void destroy_entities();
void landing_gear_callback(const void* msgin);
void mavros_battery_callback(const void* msgin);
void timer_callback(rcl_timer_t* timer, int64_t last_call_time);

// New servo control functions
void initServoStates();
void updateServoMovement();
void moveServoSequence(bool retract);
void moveServoSmoothly(int servo_index, int target_angle);
void attachAllServos();
void detachAllServos();

// External variables for battery data from MAVROS
extern float current_voltage;
extern float current_current;
extern float current_power;

// Servo control variables
extern ServoState servo_states[SERVO_COUNT];
extern bool is_sequence_active;

extern rclc_executor_t executor;

#endif
