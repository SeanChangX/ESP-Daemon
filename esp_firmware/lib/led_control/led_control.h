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

void initLED();
void setLedOverrideState(int new_mode, unsigned long timestamp_ms);
void colorWipe(uint32_t color, int wait);
void rainbow(int wait);
void LEDTask(void* pvParameters);

#endif
