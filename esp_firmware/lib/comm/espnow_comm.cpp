#include "espnow_comm.h"

#include "config.h"
#include "app_settings.h"
#include "wifi_config.h"
#if APP_MODE == APP_MODE_DAEMON
#include "ros_node.h"
#endif

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <array>
#include <cstring>

namespace {

bool g_espnowReady = false;

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

#if APP_MODE == APP_MODE_DAEMON

bool isAuthorizedEmergencySwitch(const uint8_t* mac_addr) {
  const auto& allowed = getAppSettings().emergency_switch_macs;
  for (const auto& mac : allowed) {
    if (memcmp(mac.data(), mac_addr, 6) == 0) {
      return true;
    }
  }
  return false;
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

#else

constexpr uint32_t kEStopSendIntervalMs = 80;
constexpr char kEStopPayload[] = "STOP";

bool g_estopSwitchPressed = false;
int g_estopSwitchRawLevel = HIGH;
uint32_t g_estopPacketCount = 0;
unsigned long g_lastEStopSendMs = 0;
bool g_estopPeerConfigured = false;
std::array<uint8_t, 6> g_estopPeerMac = {0, 0, 0, 0, 0, 0};
String g_estopPeerMacText;
uint8_t g_estopSwitchPinConfigured = 255;
bool g_estopSwitchActiveHighConfigured = false;

void configureEStopSwitchPin(bool forceReconfigure) {
  const AppSettings& settings = getAppSettings();
  if (!forceReconfigure &&
      settings.estop_switch_pin == g_estopSwitchPinConfigured &&
      settings.estop_switch_active_high == g_estopSwitchActiveHighConfigured) {
    return;
  }

  g_estopSwitchPinConfigured = settings.estop_switch_pin;
  g_estopSwitchActiveHighConfigured = settings.estop_switch_active_high;

  // Active-low uses INPUT_PULLUP so a pressed switch can pull the line low.
  const uint8_t mode = settings.estop_switch_active_high ? INPUT : INPUT_PULLUP;
  pinMode(settings.estop_switch_pin, mode);
}

void sampleEStopSwitch() {
  configureEStopSwitchPin(false);

  const AppSettings& settings = getAppSettings();
  const int raw = digitalRead(settings.estop_switch_pin);
  g_estopSwitchRawLevel = raw;
  g_estopSwitchPressed = (raw == (settings.estop_switch_active_high ? HIGH : LOW));
}

void clearEStopPeer(bool verboseLog) {
  if (g_estopPeerConfigured) {
    esp_now_del_peer(g_estopPeerMac.data());
  }
  g_estopPeerConfigured = false;
  g_estopPeerMac = {0, 0, 0, 0, 0, 0};
  g_estopPeerMacText = "";

  if (verboseLog) {
    DAEMON_LOGLN("ESP-NOW E-stop peer cleared");
  }
}

bool ensureEStopPeerConfigured(bool verboseLog) {
  if (!g_espnowReady) {
    return false;
  }

  String configuredMacText = getAppSettings().estop_target_mac;
  configuredMacText.trim();

  if (configuredMacText.length() == 0) {
    clearEStopPeer(verboseLog);
    return false;
  }

  std::array<uint8_t, 6> parsedMac = {};
  if (!parseMacString(configuredMacText, parsedMac)) {
    clearEStopPeer(verboseLog);
    if (verboseLog) {
      DAEMON_LOGF("ESP-NOW E-stop peer MAC invalid: %s\n", configuredMacText.c_str());
    }
    return false;
  }

  const String normalizedMacText = toMacString(parsedMac);
  if (g_estopPeerConfigured && memcmp(g_estopPeerMac.data(), parsedMac.data(), 6) == 0) {
    g_estopPeerMacText = normalizedMacText;
    return true;
  }

  clearEStopPeer(false);

  if (esp_now_is_peer_exist(parsedMac.data())) {
    esp_now_del_peer(parsedMac.data());
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, parsedMac.data(), 6);
  peerInfo.channel = 0;
  peerInfo.ifidx = WIFI_IF_STA;
  peerInfo.encrypt = false;

  const esp_err_t addErr = esp_now_add_peer(&peerInfo);
  if (addErr != ESP_OK) {
    if (verboseLog) {
      DAEMON_LOGF("ESP-NOW E-stop add peer failed (%d)\n", static_cast<int>(addErr));
    }
    return false;
  }

  g_estopPeerConfigured = true;
  g_estopPeerMac = parsedMac;
  g_estopPeerMacText = normalizedMacText;

  if (verboseLog) {
    DAEMON_LOGF("ESP-NOW E-stop peer configured: %s\n", g_estopPeerMacText.c_str());
  }

  return true;
}

void sendEStopIfPressed() {
  if (!g_espnowReady || !getAppSettings().runtime_espnow_enabled) {
    return;
  }

  if (!g_estopSwitchPressed) {
    return;
  }

  const unsigned long now = millis();
  if (now - g_lastEStopSendMs < kEStopSendIntervalMs) {
    return;
  }
  g_lastEStopSendMs = now;

  if (!ensureEStopPeerConfigured(false)) {
    return;
  }

  const esp_err_t sendErr = esp_now_send(
    g_estopPeerMac.data(),
    reinterpret_cast<const uint8_t*>(kEStopPayload),
    sizeof(kEStopPayload) - 1);

  if (sendErr == ESP_OK) {
    if (g_estopPacketCount < 0xFFFFFFFFUL) {
      g_estopPacketCount++;
    }
    return;
  }

  if (sendErr == ESP_ERR_ESPNOW_NOT_FOUND) {
    g_estopPeerConfigured = false;
  }
}

#endif

} // namespace

void initESPNow() {
  if (!getAppSettings().runtime_espnow_enabled) {
    DAEMON_LOGLN("ESP-NOW disabled by runtime settings");
#if APP_MODE == APP_MODE_ESTOP
    configureEStopSwitchPin(true);
    sampleEStopSwitch();
#endif
    return;
  }

#if APP_MODE == APP_MODE_ESTOP
  configureEStopSwitchPin(true);
  sampleEStopSwitch();
#endif

  // Keep current Wi-Fi mode when Wi-Fi manager is enabled, so NetWizard AP/portal
  // is not torn down by forcing STA mode.
  applyEspNowChannel(true);

  if (esp_now_init() != ESP_OK) {
    DAEMON_LOGLN("ESP-NOW init failed");
    g_espnowReady = false;
    return;
  }

  g_espnowReady = true;

#if APP_MODE == APP_MODE_DAEMON
  esp_now_register_recv_cb(onDataRecv);
  DAEMON_LOGF("ESP-NOW receiver initialized (allowed emergency switches: %d)\n",
              static_cast<int>(getAppSettings().emergency_switch_macs.size()));
#else
  ensureEStopPeerConfigured(true);
  DAEMON_LOGLN("ESP-NOW E-stop sender initialized");
#endif
}

void refreshESPNowChannel() {
  if (!getAppSettings().runtime_espnow_enabled) {
    return;
  }

  applyEspNowChannel(true);

#if APP_MODE == APP_MODE_ESTOP
  if (g_espnowReady) {
    ensureEStopPeerConfigured(true);
  }
#endif
}

void handleESPNow() {
#if APP_MODE == APP_MODE_DAEMON
  return;
#else
  sampleEStopSwitch();
  sendEStopIfPressed();
#endif
}

bool isEStopSwitchPressed() {
#if APP_MODE == APP_MODE_ESTOP
  return g_estopSwitchPressed;
#else
  return false;
#endif
}

int getEStopSwitchRawLevel() {
#if APP_MODE == APP_MODE_ESTOP
  return g_estopSwitchRawLevel;
#else
  return LOW;
#endif
}

uint32_t getEStopPacketCount() {
#if APP_MODE == APP_MODE_ESTOP
  return g_estopPacketCount;
#else
  return 0;
#endif
}

bool isEStopPeerConfigured() {
#if APP_MODE == APP_MODE_ESTOP
  return g_estopPeerConfigured;
#else
  return false;
#endif
}

String getEStopTargetMac() {
#if APP_MODE == APP_MODE_ESTOP
  return g_estopPeerMacText;
#else
  return String();
#endif
}
