#ifndef LED_CONTROL_H
#define LED_CONTROL_H

#include <Arduino.h>

enum led_mode {
  DEFAULT_MODE,
  BATT_LOW,
  BATT_DISCONNECTED,
  EME_ENABLE,
  EME_DISABLE,
  SIMA_CMD
};

extern volatile int mode;
extern unsigned long last_override_time;
extern unsigned long override_duration;

void initLED();
void colorWipe(uint32_t color, int wait);
void rainbow(int wait);
void LEDTask(void* pvParameters);

#endif
