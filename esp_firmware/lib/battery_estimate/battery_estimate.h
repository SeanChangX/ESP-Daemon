#ifndef BATTERY_ESTIMATE_H
#define BATTERY_ESTIMATE_H

#include <Arduino.h>
#include <ArduinoJson.h>

// Makita 18V LXT-style pack (5S Li-ion): voltage-only estimate with EMA + slope filter.
void batteryEstimateInit();
void batteryEstimateUpdate(float pack_volts, bool pack_connected);
void batteryEstimateAppendJson(JsonDocument& readings);

#endif
