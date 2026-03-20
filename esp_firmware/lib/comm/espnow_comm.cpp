#include "espnow_comm.h"

#include "config.h"
#include "app_settings.h"
#include "wifi_config.h"
#if APP_MODE == APP_MODE_DAEMON
#include "ros_node.h"
#endif

#include <WiFi.h>
#if APP_MODE == APP_MODE_ESTOP
#include <HTTPClient.h>
#endif
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
constexpr uint32_t kEStopDebounceMs = 35;
constexpr uint32_t kWledRestoreRetryIntervalMs = 1000;
constexpr uint16_t kWledHttpTimeoutMs = 1500;
constexpr size_t kWledSnapshotMaxBytes = 4096;
constexpr char kEStopPayload[] = "STOP";

bool g_estopSwitchPressed = false;
int g_estopSwitchRawLevel = HIGH;
int g_estopLastObservedRawLevel = HIGH;
unsigned long g_estopRawChangedAtMs = 0;
bool g_estopDebounceInitialized = false;
bool g_estopEdgeInitialized = false;
bool g_estopLastStablePressed = false;
uint32_t g_estopPacketCount = 0;
unsigned long g_lastEStopSendMs = 0;
bool g_estopPeerConfigured = false;
std::array<uint8_t, 6> g_estopPeerMac = {0, 0, 0, 0, 0, 0};
String g_estopPeerMacText;
uint8_t g_estopSwitchPinConfigured = 255;
bool g_estopSwitchActiveHighConfigured = false;
bool g_wledSnapshotValid = false;
bool g_wledRestorePending = false;
unsigned long g_wledLastRestoreAttemptMs = 0;
String g_wledSnapshotJson;
String g_wledStatus = "disabled";

String normalizeWledBaseUrl() {
  String url = getAppSettings().estop_wled_base_url;
  url.trim();
  if (url.endsWith("/")) {
    url.remove(url.length() - 1);
  }
  return url;
}

bool beginHttp(HTTPClient& http, const String& url) {
  http.setTimeout(kWledHttpTimeoutMs);
  return http.begin(url);
}

bool wledHttpGet(const String& url, String& responseBody) {
  HTTPClient http;
  if (!beginHttp(http, url)) {
    return false;
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  responseBody = http.getString();
  http.end();
  return true;
}

bool wledHttpPostJson(const String& url, const String& payload) {
  HTTPClient http;
  if (!beginHttp(http, url)) {
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  const int httpCode = http.POST(payload);
  const bool ok = (httpCode >= 200 && httpCode < 300);
  http.end();
  return ok;
}

bool wledControlEnabled() {
  const AppSettings& settings = getAppSettings();
  return settings.estop_wled_enabled && settings.estop_wled_base_url.length() > 0;
}

void handleWledPressedEdge() {
  if (!wledControlEnabled()) {
    g_wledStatus = "disabled";
    g_wledSnapshotValid = false;
    g_wledRestorePending = false;
    g_wledSnapshotJson = "";
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    g_wledStatus = "wifi_disconnected";
    return;
  }

  const String baseUrl = normalizeWledBaseUrl();
  if (baseUrl.length() == 0) {
    g_wledStatus = "url_empty";
    return;
  }
  const String stateUrl = baseUrl + "/json/state";

  String snapshot;
  if (!wledHttpGet(stateUrl, snapshot)) {
    g_wledStatus = "snapshot_get_failed";
    g_wledSnapshotValid = false;
    g_wledRestorePending = false;
    g_wledSnapshotJson = "";
    return;
  }

  if (snapshot.length() == 0 || snapshot.length() > kWledSnapshotMaxBytes) {
    g_wledStatus = "snapshot_invalid";
    g_wledSnapshotValid = false;
    g_wledRestorePending = false;
    g_wledSnapshotJson = "";
    return;
  }

  g_wledSnapshotJson = snapshot;
  g_wledSnapshotValid = true;
  g_wledRestorePending = false;

  JsonDocument payload;
  payload["on"] = true;
  payload["ps"] = getAppSettings().estop_wled_preset;
  String payloadText;
  serializeJson(payload, payloadText);

  if (wledHttpPostJson(stateUrl, payloadText)) {
    g_wledStatus = "estop_preset_applied";
  } else {
    g_wledStatus = "preset_apply_failed";
  }
}

void tryRestoreWledState() {
  if (!g_wledRestorePending || !g_wledSnapshotValid) {
    return;
  }

  if (g_estopSwitchPressed) {
    return;
  }

  const unsigned long now = millis();
  if ((now - g_wledLastRestoreAttemptMs) < kWledRestoreRetryIntervalMs) {
    return;
  }
  g_wledLastRestoreAttemptMs = now;

  if (WiFi.status() != WL_CONNECTED) {
    g_wledStatus = "restore_wait_wifi";
    return;
  }

  const String baseUrl = normalizeWledBaseUrl();
  if (baseUrl.length() == 0) {
    g_wledStatus = "restore_url_empty";
    return;
  }
  const String stateUrl = baseUrl + "/json/state";

  if (!wledHttpPostJson(stateUrl, g_wledSnapshotJson)) {
    g_wledStatus = "restore_failed_retrying";
    return;
  }

  g_wledStatus = "restored";
  g_wledRestorePending = false;
  g_wledSnapshotValid = false;
  g_wledSnapshotJson = "";
}

void handleWledReleasedEdge() {
  if (!wledControlEnabled()) {
    g_wledStatus = "disabled";
    g_wledSnapshotValid = false;
    g_wledRestorePending = false;
    g_wledSnapshotJson = "";
    return;
  }

  if (!g_wledSnapshotValid) {
    g_wledStatus = "no_snapshot";
    g_wledRestorePending = false;
    return;
  }

  g_wledRestorePending = true;
  g_wledLastRestoreAttemptMs = 0;
  tryRestoreWledState();
}

void handleEStopSwitchEdge(bool pressed) {
  if (pressed) {
    handleWledPressedEdge();
  } else {
    handleWledReleasedEdge();
  }
}

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

  // Reset debounce state after pin/logic changes.
  g_estopDebounceInitialized = false;
}

void sampleEStopSwitch() {
  configureEStopSwitchPin(false);

  const AppSettings& settings = getAppSettings();
  const unsigned long now = millis();
  const int raw = digitalRead(settings.estop_switch_pin);

  if (!g_estopDebounceInitialized) {
    g_estopDebounceInitialized = true;
    g_estopSwitchRawLevel = raw;
    g_estopLastObservedRawLevel = raw;
    g_estopRawChangedAtMs = now;
  }

  if (raw != g_estopLastObservedRawLevel) {
    g_estopLastObservedRawLevel = raw;
    g_estopRawChangedAtMs = now;
  }

  if ((now - g_estopRawChangedAtMs) >= kEStopDebounceMs) {
    g_estopSwitchRawLevel = g_estopLastObservedRawLevel;
  }

  g_estopSwitchPressed =
    (g_estopSwitchRawLevel == (settings.estop_switch_active_high ? HIGH : LOW));

  if (!g_estopEdgeInitialized) {
    g_estopEdgeInitialized = true;
    g_estopLastStablePressed = g_estopSwitchPressed;
    return;
  }

  if (g_estopSwitchPressed != g_estopLastStablePressed) {
    g_estopLastStablePressed = g_estopSwitchPressed;
    handleEStopSwitchEdge(g_estopSwitchPressed);
  }
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
  g_wledStatus = wledControlEnabled() ? "ready" : "disabled";
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
  tryRestoreWledState();
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

String getEStopWledStatus() {
#if APP_MODE == APP_MODE_ESTOP
  return g_wledStatus;
#else
  return String("n/a");
#endif
}

bool isEStopWledSnapshotReady() {
#if APP_MODE == APP_MODE_ESTOP
  return g_wledSnapshotValid;
#else
  return false;
#endif
}
