#ifndef VOLTMETER_H
#define VOLTMETER_H

#include <Arduino.h>

extern float Vbattf;

void initVoltmeter();
void voltmeter();
void onTimer();
void voltmeterTask(void* pvParameters);
String getSensorReadings();

#endif
