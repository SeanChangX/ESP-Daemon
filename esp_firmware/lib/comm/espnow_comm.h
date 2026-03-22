#ifndef ESPNOW_COMM_H
#define ESPNOW_COMM_H

#include <Arduino.h>
#include <stddef.h>

void initESPNow();
void refreshESPNowChannel();
void handleESPNow();

bool isEStopSwitchPressed();
int getEStopSwitchRawLevel();
uint32_t getEStopPacketCount();
bool isEStopPeerConfigured();
String getEStopTargetMac();
size_t getEStopRouteCount();
size_t getEStopPressedRouteCount();
size_t getEStopConfiguredPeerCount();
bool isEStopRoutePressed(size_t index);
int getEStopRouteRawLevel(size_t index);
bool isEStopRoutePeerConfigured(size_t index);
String getEStopRouteTargetMac(size_t index);
String getEStopRouteEffectiveTargetMac(size_t index);
String getEStopWledStatus();
bool isEStopWledSnapshotReady();

#endif
