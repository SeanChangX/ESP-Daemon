#ifndef APP_SETTINGS_INTERNAL_H
#define APP_SETTINGS_INTERNAL_H

#include "app_settings.h"

namespace app_settings_internal {

void lockSettings();
void unlockSettings();

const char* settingsNvsNamespace();
const char* settingsNvsKey();
const char* defaultControlPanelUrl();

void applyDefaultsUnlocked();
void loadFromJsonUnlocked(const JsonObjectConst& json);
bool validateControlPanelUrlUnlocked(const String& url);
void appSettingsToJsonUnlocked(JsonDocument& doc, bool include_pin_code);

AppSettings& mutableSettings();

} // namespace app_settings_internal

#endif
