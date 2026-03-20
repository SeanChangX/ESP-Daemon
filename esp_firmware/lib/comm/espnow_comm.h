#ifndef ESPNOW_COMM_H
#define ESPNOW_COMM_H

#include <Arduino.h>

void initESPNow();
void refreshESPNowChannel();
void handleESPNow();

bool isEStopSwitchPressed();
int getEStopSwitchRawLevel();
uint32_t getEStopPacketCount();
bool isEStopPeerConfigured();
String getEStopTargetMac();

#endif
