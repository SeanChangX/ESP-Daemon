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
#include <vector>

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

bool resolveEmergencyTargetsForSource(
  const uint8_t* mac_addr,
  bool& target_group1,
  bool& target_group2,
  bool& target_group3) {
  target_group1 = false;
  target_group2 = false;
  target_group3 = false;

  const auto& sources = getAppSettings().emergency_sources;
  bool found = false;
  for (const auto& src : sources) {
    if (memcmp(src.mac.data(), mac_addr, 6) != 0) {
      continue;
    }
    found = true;
    target_group1 = target_group1 || src.control_group1_enabled;
    target_group2 = target_group2 || src.control_group2_enabled;
    target_group3 = target_group3 || src.control_group3_enabled;
  }

  return found;
}

void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* incomingData, int len) {
  if (info == nullptr || incomingData == nullptr || len <= 0) {
    return;
  }

  const uint8_t* mac_addr = info->src_addr;
  bool targetGroup1 = false;
  bool targetGroup2 = false;
  bool targetGroup3 = false;
  if (!resolveEmergencyTargetsForSource(mac_addr, targetGroup1, targetGroup2, targetGroup3)) {
    return;
  }

  // Remote emergency switch is expected to repeatedly transmit "STOP".
  // Any authorized packet is treated as an emergency-stop trigger.
  triggerRemoteEmergencyStop(targetGroup1, targetGroup2, targetGroup3);

  DAEMON_LOGF("ESP-NOW emergency stop from %02X:%02X:%02X:%02X:%02X:%02X -> groups(%d,%d,%d) forced OFF\n",
              mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5],
              targetGroup1 ? 1 : 0, targetGroup2 ? 1 : 0, targetGroup3 ? 1 : 0);
}

#else

constexpr uint32_t kEStopSendIntervalMs = 80;
constexpr uint32_t kEStopDebounceMs = 35;
constexpr uint32_t kEStopPeerEnsureIntervalMs = 1000;
constexpr uint32_t kWledRestoreRetryIntervalMs = 1000;
constexpr uint16_t kWledHttpTimeoutMs = 1500;
constexpr size_t kWledSnapshotMaxBytes = 4096;
constexpr char kEStopPayload[] = "STOP";
// Cybertruck-style boot cue + standard factory E-STOP alarm (passive buzzer approximation).
constexpr uint16_t kBuzzerAlarmOnMs = 300;
constexpr uint16_t kBuzzerAlarmOffMs = 240;
constexpr uint16_t kBuzzerAlarmFreqAHz = 1150;
constexpr uint16_t kBuzzerAlarmFreqBHz = 1150;
constexpr uint16_t kBuzzerPwmAttachFreqHz = 2000;
constexpr uint8_t kBuzzerPwmResolutionBits = 8;
constexpr uint8_t kBuzzerPwmDuty = 127;

struct BuzzerToneStep {
  uint16_t holdMs;
  uint16_t freqHz;
};

constexpr BuzzerToneStep kBuzzerStartupPattern[] = {
  // Modern factory-style boot cue: short stepped rise + confirmation tail.
  {78, 880},
  {26, 0},
  {82, 1175},
  {28, 0},
  {92, 1568},
  {34, 0},
  {148, 1319}
};

struct EStopRouteRuntime {
  std::array<uint8_t, 6> targetMac = {0, 0, 0, 0, 0, 0};
  String targetMacText;
  uint8_t switchPin = 0;
  bool switchActiveHigh = false;
  bool switchLogicInverted = false;
  int rawLevel = HIGH;
  int lastObservedRawLevel = HIGH;
  unsigned long rawChangedAtMs = 0;
  bool debounceInitialized = false;
  bool pressed = false;
  bool peerConfigured = false;
};

std::vector<EStopRouteRuntime> g_estopRoutes;
String g_estopRouteSignature;
bool g_estopSwitchPressed = false;
size_t g_estopPressedRouteCount = 0;
int g_estopSwitchRawLevel = HIGH;
uint32_t g_estopPacketCount = 0;
unsigned long g_lastEStopSendMs = 0;
unsigned long g_lastPeerEnsureMs = 0;
bool g_wledSnapshotValid = false;
bool g_wledRestorePending = false;
unsigned long g_wledLastRestoreAttemptMs = 0;
String g_wledSnapshotJson;
String g_wledStatus = "disabled";

enum class BuzzerMode : uint8_t {
  Off = 0,
  StartupTone,
  Alarm
};

uint8_t g_buzzerPinConfigured = 255;
bool g_buzzerEnabledConfigured = false;
bool g_buzzerPwmAttached = false;
bool g_buzzerOutputOn = false;
BuzzerMode g_buzzerMode = BuzzerMode::Off;
bool g_buzzerAlarmUseAltTone = false;
unsigned long g_buzzerPhaseStartedAtMs = 0;
size_t g_buzzerStartupStep = 0;
bool g_startupTonePlayed = false;
bool g_wifiConnectedPrev = false;

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

void stopBuzzerOutput() {
  if (!g_buzzerEnabledConfigured || g_buzzerPinConfigured > 48) {
    return;
  }

  if (g_buzzerPwmAttached) {
    ledcWriteTone(g_buzzerPinConfigured, 0);
    ledcWrite(g_buzzerPinConfigured, 0);
  } else {
    digitalWrite(g_buzzerPinConfigured, LOW);
  }
}

void playBuzzerTone(uint16_t freqHz) {
  if (!g_buzzerEnabledConfigured || g_buzzerPinConfigured > 48) {
    return;
  }

  if (freqHz == 0) {
    stopBuzzerOutput();
    return;
  }

  if (g_buzzerPwmAttached) {
    ledcWriteTone(g_buzzerPinConfigured, freqHz);
    ledcWrite(g_buzzerPinConfigured, kBuzzerPwmDuty);
  } else {
    digitalWrite(g_buzzerPinConfigured, HIGH);
  }
}

void updateWledForAggregateSwitchState(bool pressed) {
  if (pressed) {
    if (g_buzzerEnabledConfigured) {
      g_buzzerMode = BuzzerMode::Alarm;
      g_buzzerPhaseStartedAtMs = millis();
      g_buzzerAlarmUseAltTone = false;
      g_buzzerOutputOn = true;
      playBuzzerTone(kBuzzerAlarmFreqAHz);
      g_buzzerAlarmUseAltTone = true;
    }
    handleWledPressedEdge();
    return;
  }
  if (g_buzzerMode == BuzzerMode::Alarm) {
    g_buzzerMode = BuzzerMode::Off;
    stopBuzzerOutput();
    g_buzzerOutputOn = false;
    g_buzzerStartupStep = 0;
    g_buzzerAlarmUseAltTone = false;
  }
  handleWledReleasedEdge();
}

void applyBuzzerConfig(bool forceReconfigure) {
  const AppSettings& settings = getAppSettings();
  if (!forceReconfigure &&
      settings.estop_buzzer_enabled == g_buzzerEnabledConfigured &&
      settings.estop_buzzer_pin == g_buzzerPinConfigured) {
    return;
  }

  const uint8_t prevPin = g_buzzerPinConfigured;
  if (g_buzzerEnabledConfigured && prevPin <= 48) {
    if (g_buzzerPwmAttached) {
      ledcWriteTone(prevPin, 0);
      ledcWrite(prevPin, 0);
      ledcDetach(prevPin);
    }
    pinMode(prevPin, OUTPUT);
    digitalWrite(prevPin, LOW);
  }

  g_buzzerEnabledConfigured = settings.estop_buzzer_enabled;
  g_buzzerPinConfigured = settings.estop_buzzer_pin;
  g_buzzerPwmAttached = false;
  g_buzzerOutputOn = false;
  g_buzzerMode = BuzzerMode::Off;
  g_buzzerAlarmUseAltTone = false;
  g_buzzerStartupStep = 0;

  if (!g_buzzerEnabledConfigured || g_buzzerPinConfigured > 48) {
    g_buzzerEnabledConfigured = false;
    return;
  }

  pinMode(g_buzzerPinConfigured, OUTPUT);
  digitalWrite(g_buzzerPinConfigured, LOW);
  g_buzzerPwmAttached = ledcAttach(g_buzzerPinConfigured, kBuzzerPwmAttachFreqHz, kBuzzerPwmResolutionBits);
  if (!g_buzzerPwmAttached) {
    DAEMON_LOGF("Buzzer PWM attach failed on GPIO %u; fallback to digital pulses\n",
                static_cast<unsigned>(g_buzzerPinConfigured));
  }
}

void startStartupTone() {
  if (!g_buzzerEnabledConfigured) {
    return;
  }

  const size_t startupLen = sizeof(kBuzzerStartupPattern) / sizeof(kBuzzerStartupPattern[0]);
  if (startupLen == 0) {
    return;
  }

  g_buzzerMode = BuzzerMode::StartupTone;
  g_buzzerStartupStep = 0;
  g_buzzerPhaseStartedAtMs = millis();
  g_buzzerOutputOn = (kBuzzerStartupPattern[0].freqHz > 0);
  playBuzzerTone(kBuzzerStartupPattern[0].freqHz);
}

void updateBuzzer() {
  applyBuzzerConfig(false);

  const bool wifiConnected = (WiFi.status() == WL_CONNECTED);
  if (!g_startupTonePlayed && wifiConnected && !g_wifiConnectedPrev) {
    // Safety priority: never replace an active E-STOP alarm with startup tone.
    if (!g_estopSwitchPressed && g_buzzerMode != BuzzerMode::Alarm) {
      startStartupTone();
    }
    g_startupTonePlayed = true;
  }
  g_wifiConnectedPrev = wifiConnected;

  if (!g_buzzerEnabledConfigured) {
    return;
  }

  const unsigned long now = millis();
  if (g_buzzerMode == BuzzerMode::Alarm) {
    const uint16_t holdMs = g_buzzerOutputOn ? kBuzzerAlarmOnMs : kBuzzerAlarmOffMs;
    if ((now - g_buzzerPhaseStartedAtMs) >= holdMs) {
      g_buzzerOutputOn = !g_buzzerOutputOn;
      g_buzzerPhaseStartedAtMs = now;
      if (g_buzzerOutputOn) {
        const uint16_t freq = g_buzzerAlarmUseAltTone ? kBuzzerAlarmFreqBHz : kBuzzerAlarmFreqAHz;
        g_buzzerAlarmUseAltTone = !g_buzzerAlarmUseAltTone;
        playBuzzerTone(freq);
      } else {
        stopBuzzerOutput();
      }
    }
    return;
  }

  if (g_buzzerMode == BuzzerMode::StartupTone) {
    const size_t startupLen = sizeof(kBuzzerStartupPattern) / sizeof(kBuzzerStartupPattern[0]);
    if (g_buzzerStartupStep >= startupLen) {
      g_buzzerMode = BuzzerMode::Off;
      g_buzzerOutputOn = false;
      stopBuzzerOutput();
      return;
    }

    const uint16_t holdMs = kBuzzerStartupPattern[g_buzzerStartupStep].holdMs;
    if ((now - g_buzzerPhaseStartedAtMs) < holdMs) {
      return;
    }

    g_buzzerStartupStep++;
    g_buzzerPhaseStartedAtMs = now;

    if (g_buzzerStartupStep >= startupLen) {
      g_buzzerMode = BuzzerMode::Off;
      g_buzzerOutputOn = false;
      stopBuzzerOutput();
      return;
    }

    const uint16_t freq = kBuzzerStartupPattern[g_buzzerStartupStep].freqHz;
    g_buzzerOutputOn = (freq > 0);
    playBuzzerTone(freq);
  }
}

String buildRouteSignature() {
  String signature;
  signature.reserve(64);
  const auto& routes = getAppSettings().estop_routes;
  for (const auto& route : routes) {
    signature += route.target_mac;
    signature += "|";
    signature += String(route.switch_pin);
    signature += "|";
    signature += route.switch_active_high ? "1" : "0";
    signature += "|";
    signature += route.switch_logic_inverted ? "1" : "0";
    signature += ";";
  }
  return signature;
}

void clearRoutePeers() {
  if (!g_espnowReady) {
    for (auto& route : g_estopRoutes) {
      route.peerConfigured = false;
    }
    return;
  }

  for (auto& route : g_estopRoutes) {
    if (route.peerConfigured && route.targetMacText.length() > 0) {
      esp_now_del_peer(route.targetMac.data());
    }
    route.peerConfigured = false;
  }
}

void configureRoutePin(const EStopRouteRuntime& route) {
  // Use internal pulls in both polarities to avoid floating GPIO noise:
  // - active-low  -> INPUT_PULLUP  (pressed shorts to GND)
  // - active-high -> INPUT_PULLDOWN (pressed drives to 3V3)
  const uint8_t mode = route.switchActiveHigh ? INPUT_PULLDOWN : INPUT_PULLUP;
  pinMode(route.switchPin, mode);
}

bool ensureRoutePeerConfigured(EStopRouteRuntime& route, bool verboseLog) {
  if (!g_espnowReady) {
    route.peerConfigured = false;
    return false;
  }

  if (route.targetMacText.length() == 0) {
    route.peerConfigured = false;
    return false;
  }

  if (esp_now_is_peer_exist(route.targetMac.data())) {
    route.peerConfigured = true;
    return true;
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, route.targetMac.data(), 6);
  peerInfo.channel = 0;
  peerInfo.ifidx = WIFI_IF_STA;
  peerInfo.encrypt = false;

  const esp_err_t addErr = esp_now_add_peer(&peerInfo);
  if (addErr != ESP_OK) {
    route.peerConfigured = false;
    if (verboseLog) {
      DAEMON_LOGF("ESP-NOW E-stop add peer failed (%d) for %s\n",
                  static_cast<int>(addErr), route.targetMacText.c_str());
    }
    return false;
  }

  route.peerConfigured = true;
  if (verboseLog) {
    DAEMON_LOGF("ESP-NOW E-stop peer configured: %s\n", route.targetMacText.c_str());
  }
  return true;
}

void ensureAllRoutePeersConfigured(bool verboseLog) {
  for (auto& route : g_estopRoutes) {
    ensureRoutePeerConfigured(route, verboseLog);
  }
}

void rebuildEStopRoutesFromSettings(bool verboseLog) {
  const String signature = buildRouteSignature();
  if (signature == g_estopRouteSignature) {
    return;
  }

  clearRoutePeers();
  g_estopRoutes.clear();

  for (const auto& routeCfg : getAppSettings().estop_routes) {
    EStopRouteRuntime route = {};
    route.switchPin = routeCfg.switch_pin;
    route.switchActiveHigh = routeCfg.switch_active_high;
    route.switchLogicInverted = routeCfg.switch_logic_inverted;
    route.rawLevel = route.switchActiveHigh ? LOW : HIGH;
    route.lastObservedRawLevel = route.rawLevel;

    std::array<uint8_t, 6> parsedMac = {};
    if (parseMacString(routeCfg.target_mac, parsedMac)) {
      route.targetMac = parsedMac;
      route.targetMacText = toMacString(parsedMac);
    }

    configureRoutePin(route);
    g_estopRoutes.push_back(route);
  }

  g_estopSwitchPressed = false;
  g_estopPressedRouteCount = 0;
  g_estopSwitchRawLevel = HIGH;
  g_estopRouteSignature = signature;
  g_lastPeerEnsureMs = 0;

  if (g_espnowReady) {
    ensureAllRoutePeersConfigured(verboseLog);
  }

  if (verboseLog) {
    DAEMON_LOGF("ESP-NOW E-stop routes loaded: %d\n", static_cast<int>(g_estopRoutes.size()));
  }
}

void sampleEStopSwitches() {
  rebuildEStopRoutesFromSettings(false);
  if (g_estopRoutes.empty()) {
    g_estopSwitchPressed = false;
    g_estopPressedRouteCount = 0;
    g_estopSwitchRawLevel = HIGH;
    return;
  }

  const unsigned long now = millis();
  size_t pressedCount = 0;

  for (auto& route : g_estopRoutes) {
    const int raw = digitalRead(route.switchPin);

    if (!route.debounceInitialized) {
      route.debounceInitialized = true;
      route.rawLevel = raw;
      route.lastObservedRawLevel = raw;
      route.rawChangedAtMs = now;
    }

    if (raw != route.lastObservedRawLevel) {
      route.lastObservedRawLevel = raw;
      route.rawChangedAtMs = now;
    }

    if ((now - route.rawChangedAtMs) >= kEStopDebounceMs) {
      route.rawLevel = route.lastObservedRawLevel;
    }

    const bool basePressed = (route.rawLevel == (route.switchActiveHigh ? HIGH : LOW));
    route.pressed = route.switchLogicInverted ? !basePressed : basePressed;
    if (route.pressed) {
      pressedCount++;
    }
  }

  const bool previousPressed = g_estopSwitchPressed;
  g_estopPressedRouteCount = pressedCount;
  g_estopSwitchPressed = (pressedCount > 0);
  g_estopSwitchRawLevel = g_estopRoutes.front().rawLevel;

  if (previousPressed != g_estopSwitchPressed) {
    updateWledForAggregateSwitchState(g_estopSwitchPressed);
  }

  if (g_espnowReady && (now - g_lastPeerEnsureMs) >= kEStopPeerEnsureIntervalMs) {
    g_lastPeerEnsureMs = now;
    ensureAllRoutePeersConfigured(false);
  }
}

bool hasMacInList(const std::vector<std::array<uint8_t, 6>>& macs, const std::array<uint8_t, 6>& target) {
  for (const auto& mac : macs) {
    if (memcmp(mac.data(), target.data(), 6) == 0) {
      return true;
    }
  }
  return false;
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

  std::vector<std::array<uint8_t, 6>> sentTargets;
  sentTargets.reserve(g_estopRoutes.size());

  for (auto& route : g_estopRoutes) {
    if (!route.pressed) {
      continue;
    }
    if (!ensureRoutePeerConfigured(route, false)) {
      continue;
    }
    if (hasMacInList(sentTargets, route.targetMac)) {
      continue;
    }

    const esp_err_t sendErr = esp_now_send(
      route.targetMac.data(),
      reinterpret_cast<const uint8_t*>(kEStopPayload),
      sizeof(kEStopPayload) - 1);

    if (sendErr == ESP_OK) {
      sentTargets.push_back(route.targetMac);
      if (g_estopPacketCount < 0xFFFFFFFFUL) {
        g_estopPacketCount++;
      }
      continue;
    }

    if (sendErr == ESP_ERR_ESPNOW_NOT_FOUND) {
      route.peerConfigured = false;
    }
  }
}

size_t countConfiguredPeers() {
  size_t count = 0;
  for (const auto& route : g_estopRoutes) {
    if (route.peerConfigured && route.targetMacText.length() > 0) {
      count++;
    }
  }
  return count;
}

String firstConfiguredPeerMac() {
  for (const auto& route : g_estopRoutes) {
    if (route.peerConfigured && route.targetMacText.length() > 0) {
      return route.targetMacText;
    }
  }
  return String();
}

String configuredOrRequestedMac(size_t index) {
  if (index >= g_estopRoutes.size()) {
    return String();
  }
  return g_estopRoutes[index].targetMacText;
}

String configuredPeerMac(size_t index) {
  if (index >= g_estopRoutes.size()) {
    return String();
  }
  if (!g_estopRoutes[index].peerConfigured) {
    return String();
  }
  return g_estopRoutes[index].targetMacText;
}

#endif

} // namespace

void initESPNow() {
  if (!getAppSettings().runtime_espnow_enabled) {
    DAEMON_LOGLN("ESP-NOW disabled by runtime settings");
#if APP_MODE == APP_MODE_ESTOP
    applyBuzzerConfig(true);
    rebuildEStopRoutesFromSettings(true);
    sampleEStopSwitches();
    updateBuzzer();
#endif
    return;
  }

#if APP_MODE == APP_MODE_ESTOP
  applyBuzzerConfig(true);
  rebuildEStopRoutesFromSettings(true);
  sampleEStopSwitches();
  updateBuzzer();
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
  DAEMON_LOGF("ESP-NOW receiver initialized (configured emergency sources: %d)\n",
              static_cast<int>(getAppSettings().emergency_sources.size()));
#else
  ensureAllRoutePeersConfigured(true);
  DAEMON_LOGF("ESP-NOW E-stop sender initialized (routes=%d)\n", static_cast<int>(g_estopRoutes.size()));
#endif
}

void refreshESPNowChannel() {
  if (!getAppSettings().runtime_espnow_enabled) {
    return;
  }

  applyEspNowChannel(true);

#if APP_MODE == APP_MODE_ESTOP
  applyBuzzerConfig(false);
  rebuildEStopRoutesFromSettings(true);
  if (g_espnowReady) {
    ensureAllRoutePeersConfigured(true);
  }
#endif
}

void handleESPNow() {
#if APP_MODE == APP_MODE_DAEMON
  return;
#else
  sampleEStopSwitches();
  sendEStopIfPressed();
  updateBuzzer();
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
  return countConfiguredPeers() > 0;
#else
  return false;
#endif
}

String getEStopTargetMac() {
#if APP_MODE == APP_MODE_ESTOP
  return firstConfiguredPeerMac();
#else
  return String();
#endif
}

size_t getEStopRouteCount() {
#if APP_MODE == APP_MODE_ESTOP
  return g_estopRoutes.size();
#else
  return 0;
#endif
}

size_t getEStopPressedRouteCount() {
#if APP_MODE == APP_MODE_ESTOP
  return g_estopPressedRouteCount;
#else
  return 0;
#endif
}

size_t getEStopConfiguredPeerCount() {
#if APP_MODE == APP_MODE_ESTOP
  return countConfiguredPeers();
#else
  return 0;
#endif
}

bool isEStopRoutePressed(size_t index) {
#if APP_MODE == APP_MODE_ESTOP
  if (index >= g_estopRoutes.size()) {
    return false;
  }
  return g_estopRoutes[index].pressed;
#else
  (void)index;
  return false;
#endif
}

int getEStopRouteRawLevel(size_t index) {
#if APP_MODE == APP_MODE_ESTOP
  if (index >= g_estopRoutes.size()) {
    return LOW;
  }
  return g_estopRoutes[index].rawLevel;
#else
  (void)index;
  return LOW;
#endif
}

bool isEStopRoutePeerConfigured(size_t index) {
#if APP_MODE == APP_MODE_ESTOP
  if (index >= g_estopRoutes.size()) {
    return false;
  }
  return g_estopRoutes[index].peerConfigured;
#else
  (void)index;
  return false;
#endif
}

String getEStopRouteTargetMac(size_t index) {
#if APP_MODE == APP_MODE_ESTOP
  return configuredOrRequestedMac(index);
#else
  (void)index;
  return String();
#endif
}

String getEStopRouteEffectiveTargetMac(size_t index) {
#if APP_MODE == APP_MODE_ESTOP
  return configuredPeerMac(index);
#else
  (void)index;
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
