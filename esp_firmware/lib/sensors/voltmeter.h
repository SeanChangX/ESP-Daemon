#ifndef VOLTMETER_H
#define VOLTMETER_H

#include <Arduino.h>

enum BatteryPackStatus {
  BATTERY_STATUS_NORMAL       = 0,
  BATTERY_STATUS_LOW          = 1,
  BATTERY_STATUS_DISCONNECTED = 2
};

void initVoltmeter();
void voltmeter();
void onTimer();
void voltmeterTask(void* pvParameters);
float getBatteryVoltage();
BatteryPackStatus getBatteryPackStatus();

#endif
