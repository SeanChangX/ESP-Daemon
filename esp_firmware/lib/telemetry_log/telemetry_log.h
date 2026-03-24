#ifndef TELEMETRY_LOG_H
#define TELEMETRY_LOG_H

#include <Arduino.h>

// Battery-discharge session recorder:
// - records 1 Hz while the battery is connected
// - stops on disconnect (keeps data for download until next battery connect)
// - clears and starts a new session on the next battery connect edge
void telemetryLogInit();
void telemetryLogMaybePush(float voltage_v, bool pack_connected);
String telemetryLogGetJson(bool full = false, size_t max_points = 240);

#endif
