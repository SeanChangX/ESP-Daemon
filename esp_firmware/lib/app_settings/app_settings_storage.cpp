#include "app_settings_internal.h"
#include "config.h"

#include <Preferences.h>
#include <nvs_flash.h>

namespace {

class ScopedSettingsLock {
public:
  ScopedSettingsLock() {
    app_settings_internal::lockSettings();
  }

  ~ScopedSettingsLock() {
    app_settings_internal::unlockSettings();
  }

  ScopedSettingsLock(const ScopedSettingsLock&) = delete;
  ScopedSettingsLock& operator=(const ScopedSettingsLock&) = delete;
};

} // namespace

bool saveAppSettings() {
  ScopedSettingsLock lock;

  JsonDocument doc;
  app_settings_internal::appSettingsToJsonUnlocked(doc, true);

  String payload;
  if (serializeJson(doc, payload) == 0 || payload.length() == 0) {
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(app_settings_internal::settingsNvsNamespace(), false)) {
    return false;
  }
  const size_t written = prefs.putString(app_settings_internal::settingsNvsKey(), payload);
  prefs.end();
  return written > 0;
}

bool resetAppSettingsToDefaults() {
  ScopedSettingsLock lock;
  app_settings_internal::applyDefaultsUnlocked();
  return saveAppSettings();
}

bool eraseAppSettingsFromNvs() {
  ScopedSettingsLock lock;

  const esp_err_t deinitErr = nvs_flash_deinit();
  if (deinitErr != ESP_OK && deinitErr != ESP_ERR_NVS_NOT_INITIALIZED) {
    return false;
  }

  if (nvs_flash_erase() != ESP_OK) {
    return false;
  }

  if (nvs_flash_init() != ESP_OK) {
    return false;
  }

  // Keep runtime consistent with wiped storage until next reboot.
  // Note: this now erases the whole NVS partition (including Wi-Fi creds).
  app_settings_internal::applyDefaultsUnlocked();
  return true;
}

void initAppSettings(bool storage_available) {
  (void)storage_available;
  ScopedSettingsLock lock;

  app_settings_internal::applyDefaultsUnlocked();
  bool loaded = false;

  // Primary storage: NVS (Preferences). Survives SPIFFS uploads.
  Preferences prefs;
  if (prefs.begin(app_settings_internal::settingsNvsNamespace(), true)) {
    const String payload = prefs.getString(app_settings_internal::settingsNvsKey(), "");
    prefs.end();

    if (payload.length() > 0) {
      JsonDocument doc;
      const DeserializationError err = deserializeJson(doc, payload);
      if (err) {
        DAEMON_LOGF("Failed to parse NVS settings (%s), using defaults\n", err.c_str());
      } else if (!doc.is<JsonObject>()) {
        DAEMON_LOGLN("Invalid NVS settings root, using defaults");
      } else {
        app_settings_internal::loadFromJsonUnlocked(doc.as<JsonObjectConst>());
        loaded = true;
      }
    } else {
      DAEMON_LOGLN("NVS settings missing, using defaults");
    }
  } else {
    DAEMON_LOGLN("Failed to open NVS settings namespace, using defaults");
  }

  if (!loaded) {
    DAEMON_LOGLN("Using in-memory default settings");
  }

  AppSettings& settings = app_settings_internal::mutableSettings();
  if (!app_settings_internal::validateControlPanelUrlUnlocked(settings.control_panel_url)) {
    settings.control_panel_url = app_settings_internal::defaultControlPanelUrl();
  }
}
