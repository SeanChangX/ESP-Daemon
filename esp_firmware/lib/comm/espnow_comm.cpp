#include "espnow_comm.h"

#include "config.h"
#include "app_settings.h"
#include "ros_node.h"
#include "wifi_config.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

namespace {

bool isAuthorizedEmergencySwitch(const uint8_t* mac_addr) {
  const auto& allowed = getAppSettings().emergency_switch_macs;
  for (const auto& mac : allowed) {
    if (memcmp(mac.data(), mac_addr, 6) == 0) {
      return true;
    }
  }
  return false;
}

int resolveEffectiveEspNowChannel() {
  const bool wifiConnected = (WiFi.status() == WL_CONNECTED);
  if (wifiConnected) {
    return WiFi.channel();
  }
  return static_cast<int>(getAppSettings().espnow_channel);
}

void applyEspNowChannel(bool verboseLog) {
  const bool wifiConnected = (WiFi.status() == WL_CONNECTED);
  const int effectiveChannel = resolveEffectiveEspNowChannel();
  wifi_channel = effectiveChannel;

  if (verboseLog) {
    DAEMON_LOGF("ESP-NOW channel policy -> channel=%d (wifi_connected=%s)\n",
                effectiveChannel, wifiConnected ? "true" : "false");
  }

  if (wifiConnected) {
    return;
  }

  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool apActive = (wifiMode & WIFI_AP) != 0;
  if (apActive) {
    if (verboseLog) {
      DAEMON_LOGLN("ESP-NOW channel apply skipped while AP mode is active");
    }
    return;
  }

  const esp_err_t channelErr = esp_wifi_set_channel(static_cast<uint8_t>(effectiveChannel), WIFI_SECOND_CHAN_NONE);
  if (channelErr != ESP_OK) {
    DAEMON_LOGF("ESP-NOW channel apply failed (%d)\n", static_cast<int>(channelErr));
  }
}

void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* incomingData, int len) {
  if (info == nullptr || incomingData == nullptr || len <= 0) {
    return;
  }

  const uint8_t* mac_addr = info->src_addr;
  if (!isAuthorizedEmergencySwitch(mac_addr)) {
    return;
  }

  // Remote emergency switch is expected to repeatedly transmit "STOP".
  // Any authorized packet is treated as an emergency-stop trigger.
  triggerRemoteEmergencyStop();

  DAEMON_LOGF("ESP-NOW emergency stop from %02X:%02X:%02X:%02X:%02X:%02X -> CHASSIS DISABLED\n",
              mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
}

} // namespace

void initESPNow() {
  if (!getAppSettings().runtime_espnow_enabled) {
    DAEMON_LOGLN("ESP-NOW disabled by runtime settings");
    return;
  }

  // Keep current Wi-Fi mode when Wi-Fi manager is enabled, so NetWizard AP/portal
  // is not torn down by forcing STA mode.
  applyEspNowChannel(true);

  if (esp_now_init() != ESP_OK) {
    DAEMON_LOGLN("ESP-NOW init failed");
    return;
  }

  esp_now_register_recv_cb(onDataRecv);
  DAEMON_LOGF("ESP-NOW receiver initialized (allowed emergency switches: %d)\n",
              static_cast<int>(getAppSettings().emergency_switch_macs.size()));
}

void refreshESPNowChannel() {
  if (!getAppSettings().runtime_espnow_enabled) {
    return;
  }
  applyEspNowChannel(true);
}
