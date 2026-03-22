#ifndef ROS_NODE_H
#define ROS_NODE_H

#include <Arduino.h>
#include "config.h"

#if ENABLE_MICROROS
#include <rcl/rcl.h>
#include <rclc/rclc.h>
#include <rclc/executor.h>
#include <std_msgs/msg/bool.h>
#include <std_msgs/msg/int32.h>
#include <std_msgs/msg/float32.h>
#define EXECUTE_EVERY_N_MS(MS, X)  do { \
    static volatile int64_t init = -1; \
    if (init == -1) { init = uxr_millis();} \
    if (uxr_millis() - init > MS) { X; init = uxr_millis();} \
} while (0)
#endif

enum AgentState {
  WAITING_AGENT,
  AGENT_AVAILABLE,
  AGENT_CONNECTED,
  AGENT_DISCONNECTED
};

enum PowerControlChannel {
  POWER_CHANNEL_GROUP1 = 0,
  POWER_CHANNEL_GROUP2 = 1,
  POWER_CHANNEL_GROUP3 = 2
};

extern AgentState state;

void initROS();
#if ENABLE_MICROROS
bool create_entities();
void destroy_entities();
void control_group1_enable_callback(const void* msgin);
void control_group2_enable_callback(const void* msgin);
void control_group3_enable_callback(const void* msgin);
void timer_callback(rcl_timer_t* timer, int64_t last_call_time);
#endif
void updatePowerControls();
void setPowerControlOverride(PowerControlChannel channel, bool enabled);
void triggerRemoteEmergencyStop(bool target_group1, bool target_group2, bool target_group3);
bool getPowerControlState(PowerControlChannel channel);
bool getPhysicalSwitchState(PowerControlChannel channel);
int getPhysicalSwitchRawLevel(PowerControlChannel channel);

#if ENABLE_MICROROS
extern rclc_executor_t executor;
#endif

#endif
