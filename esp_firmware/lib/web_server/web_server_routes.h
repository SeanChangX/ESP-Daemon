#ifndef WEB_SERVER_ROUTES_H
#define WEB_SERVER_ROUTES_H

void handlePowerPost();
void handleEmergencyPost();

void handleSettingsReadPost();
void handleSettingsPost();
void handleSettingsExportPost();
void handleSettingsImportPost();
void handleSettingsUnlockPost();
void handleSettingsRestoreDefaultsPost();
void handleSettingsFactoryResetPost();

void handleEStopSettingsReadPost();
void handleEStopSettingsPost();
void handleEStopSettingsExportPost();
void handleEStopSettingsImportPost();
void handleEStopSettingsRestoreDefaultsPost();
void handleEStopSettingsFactoryResetPost();

void handleEspRebootPost();

#endif
