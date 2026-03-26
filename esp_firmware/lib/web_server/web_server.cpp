#include "web_server.h"
#include "web_server_routes.h"

#include "config.h"
#include "app_settings.h"
#include "battery_estimate.h"
#include "espnow_comm.h"
#include "led_control.h"
#include "ros_node.h"
#include "telemetry_log.h"
#include "voltmeter.h"
#include "wifi_config.h"

#include <SPIFFS.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <ElegantOTA.h>

WebServer server(80);

static void redirectToRoot() {
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

static String getContentType(const String& path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css"))  return "text/css";
  if (path.endsWith(".js"))   return "application/javascript";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".png"))  return "image/png";
  if (path.endsWith(".ico"))  return "image/x-icon";
  return "text/plain";
}

#if APP_MODE == APP_MODE_ESTOP
static bool isCrossModeUiAssetPath(const String& path) {
  return path == "/index.html" ||
         path == "/script.js" ||
         path == "/settings.html" ||
         path == "/settings.js";
}
#else
static bool isCrossModeUiAssetPath(const String& path) {
  return path == "/estop.html" ||
         path == "/estop.js";
}
#endif

static String getDeviceNameForMdns() {
  AppSettingsReadGuard settingsGuard;
  return settingsGuard.settings().device_name;
}

static void handleDeviceInfoGet() {
  JsonDocument doc;
  doc["version"] = ESP_DAEMON_FW_VERSION;

  uint8_t macBytes[6] = {0};
  WiFi.macAddress(macBytes);
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X-%02X-%02X-%02X-%02X-%02X",
           macBytes[0], macBytes[1], macBytes[2], macBytes[3], macBytes[4], macBytes[5]);
  doc["mac"] = macStr;
  AppSettingsReadGuard settingsGuard;
  const AppSettings& settings = settingsGuard.settings();
  doc["controlPanelUrl"] = settings.control_panel_url;
#if APP_MODE == APP_MODE_ESTOP
  doc["settingsPinRequired"] = false;
#else
  doc["settingsPinRequired"] = settings.pin_protection_enabled;
#endif

  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

static void handleEStopStatusGet() {
  AppSettingsReadGuard settingsGuard;
  const AppSettings& settings = settingsGuard.settings();

  JsonDocument doc;
  doc["pressed"]                 = isEStopSwitchPressed();
  doc["rawLevel"]                = getEStopSwitchRawLevel();
  doc["switchPin"]               = settings.estop_switch_pin;
  doc["switchActiveHigh"]        = settings.estop_switch_active_high;
  doc["switchLogicInverted"]     = settings.estop_switch_logic_inverted;
  doc["targetMac"]               = settings.estop_target_mac;
  doc["targetPeerConfigured"]    = isEStopPeerConfigured();
  doc["effectiveTargetMac"]      = getEStopTargetMac();
  doc["espNowChannel"]           = wifi_channel;
  doc["packetsSent"]             = getEStopPacketCount();
  doc["wledStatus"]              = getEStopWledStatus();
  doc["wledSnapshotReady"]       = isEStopWledSnapshotReady();
  doc["routeCount"]              = settings.estop_routes.size();
  doc["pressedRouteCount"]       = getEStopPressedRouteCount();
  doc["configuredPeerCount"]     = getEStopConfiguredPeerCount();

  JsonArray routes = doc["routes"].to<JsonArray>();
  const size_t routeCount = settings.estop_routes.size();
  for (size_t i = 0; i < routeCount; ++i) {
    JsonObject route = routes.add<JsonObject>();
    route["index"]                = i;
    route["switchPin"]            = settings.estop_routes[i].switch_pin;
    route["switchActiveHigh"]     = settings.estop_routes[i].switch_active_high;
    route["switchLogicInverted"]  = settings.estop_routes[i].switch_logic_inverted;
    route["targetMac"]            = settings.estop_routes[i].target_mac;
    route["pressed"]              = isEStopRoutePressed(i);
    route["rawLevel"]             = getEStopRouteRawLevel(i);
    route["peerConfigured"]       = isEStopRoutePeerConfigured(i);
    route["effectiveTargetMac"]   = getEStopRouteEffectiveTargetMac(i);
  }

  String output;
  serializeJson(doc, output);
  server.send(200, "application/json", output);
}

static String buildSensorReadingsResponse() {
  AppSettingsReadGuard settingsGuard;
  const AppSettings& settings = settingsGuard.settings();
  const float batteryVoltage = getBatteryVoltage();

  JsonDocument readings;
  readings["sensor"]                  = String(batteryVoltage);
  readings["GND"]                     = 0;
  readings["controlGroup1Power"]      = getPowerControlState(POWER_CHANNEL_GROUP1);
  readings["controlGroup2Power"]      = getPowerControlState(POWER_CHANNEL_GROUP2);
  readings["controlGroup3Power"]      = getPowerControlState(POWER_CHANNEL_GROUP3);
  readings["controlGroup1Switch"]     = getPhysicalSwitchState(POWER_CHANNEL_GROUP1);
  readings["controlGroup2Switch"]     = getPhysicalSwitchState(POWER_CHANNEL_GROUP2);
  readings["controlGroup3Switch"]     = getPhysicalSwitchState(POWER_CHANNEL_GROUP3);
  readings["controlGroup1SwitchRaw"]  = getPhysicalSwitchRawLevel(POWER_CHANNEL_GROUP1);
  readings["controlGroup2SwitchRaw"]  = getPhysicalSwitchRawLevel(POWER_CHANNEL_GROUP2);
  readings["controlGroup3SwitchRaw"]  = getPhysicalSwitchRawLevel(POWER_CHANNEL_GROUP3);
  readings["controlGroup1Name"]       = settings.control_group1_name;
  readings["controlGroup2Name"]       = settings.control_group2_name;
  readings["controlGroup3Name"]       = settings.control_group3_name;
  readings["controlGroup1SwitchPin"]  = settings.control_group1_switch_pin;
  readings["controlGroup2SwitchPin"]  = settings.control_group2_switch_pin;
  readings["controlGroup3SwitchPin"]  = settings.control_group3_switch_pin;

  switch (getBatteryPackStatus()) {
    case BATTERY_STATUS_DISCONNECTED:
      readings["batteryStatus"] = "DISCONNECTED";
      break;
    case BATTERY_STATUS_LOW:
      readings["batteryStatus"] = "LOW";
      break;
    case BATTERY_STATUS_NORMAL:
    default:
      readings["batteryStatus"] = "NORMAL";
      break;
  }

  batteryEstimateAppendJson(readings);

  switch (state) {
    case WAITING_AGENT:
      readings["microROS"] = "WAITING_AGENT";
      break;
    case AGENT_AVAILABLE:
      readings["microROS"] = "AGENT_AVAILABLE";
      break;
    case AGENT_CONNECTED:
      readings["microROS"] = "AGENT_CONNECTED";
      break;
    case AGENT_DISCONNECTED:
      readings["microROS"] = "AGENT_DISCONNECTED";
      break;
    default:
      readings["microROS"] = "UNKNOWN";
      break;
  }

  String output;
  serializeJson(readings, output);
  return output;
}

static bool sendFileFromSPIFFS(const String& path) {
  if (!SPIFFS.exists(path)) {
    return false;
  }

  File file = SPIFFS.open(path, FILE_READ);
  if (!file) {
    return false;
  }

  server.streamFile(file, getContentType(path));
  file.close();
  return true;
}

void initSPIFFS() {
  if (!SPIFFS.begin()) {
    DAEMON_LOGLN("SPIFFS Mount Failed");
  }
}

void initWebServer() {
  server.on("/", HTTP_GET, []() {
#if APP_MODE == APP_MODE_ESTOP
    if (!sendFileFromSPIFFS("/estop.html")) {
      server.send(404, "text/plain", "estop.html not found");
    }
#else
    if (!sendFileFromSPIFFS("/index.html")) {
      server.send(404, "text/plain", "index.html not found");
    }
#endif
  });

  server.on("/health", HTTP_GET, []() {
    server.send(200, "text/plain", "ok");
  });

  server.on("/device", HTTP_GET, handleDeviceInfoGet);
#if APP_MODE == APP_MODE_ESTOP
  server.on("/estop/status", HTTP_GET, handleEStopStatusGet);
#else
  server.on("/readings", HTTP_GET, []() {
    server.send(200, "application/json", buildSensorReadingsResponse());
  });

  server.on("/telemetry", HTTP_GET, []() {
    bool full = false;
    size_t max_points = 240u;
    if (server.hasArg("full")) {
      const String arg = server.arg("full");
      full = (arg == "1" || arg == "true" || arg == "TRUE");
    }
    if (server.hasArg("maxPoints")) {
      const long requested = server.arg("maxPoints").toInt();
      if (requested > 0) {
        long clamped = requested;
        if (clamped < 64) {
          clamped = 64;
        } else if (clamped > 720) {
          clamped = 720;
        }
        max_points = static_cast<size_t>(clamped);
      }
    }
    server.send(200, "application/json", telemetryLogGetJson(full, max_points));
  });

  server.on("/power",     HTTP_POST, handlePowerPost);
  server.on("/emergency", HTTP_POST, handleEmergencyPost);
#endif

#if APP_MODE == APP_MODE_ESTOP
  server.on("/estop/settings/read",           HTTP_POST, handleEStopSettingsReadPost);
  server.on("/estop/settings",                HTTP_POST, handleEStopSettingsPost);
  server.on("/estop/settings/export",         HTTP_POST, handleEStopSettingsExportPost);
  server.on("/estop/settings/import",         HTTP_POST, handleEStopSettingsImportPost);
  server.on("/estop/settings/reset",          HTTP_POST, handleEStopSettingsRestoreDefaultsPost);
  server.on("/estop/settings/factory-reset",  HTTP_POST, handleEStopSettingsFactoryResetPost);
  server.on("/estop/settings/read",           HTTP_GET,  redirectToRoot);
  server.on("/estop/settings",                HTTP_GET,  redirectToRoot);
  server.on("/estop/settings/export",         HTTP_GET,  redirectToRoot);
  server.on("/estop/settings/import",         HTTP_GET,  redirectToRoot);
  server.on("/estop/settings/reset",          HTTP_GET,  redirectToRoot);
  server.on("/estop/settings/factory-reset",  HTTP_GET,  redirectToRoot);
#else
  server.on("/power",                         HTTP_GET,  redirectToRoot);
  server.on("/emergency",                     HTTP_GET,  redirectToRoot);
  server.on("/settings/read",                 HTTP_GET,  redirectToRoot);
  server.on("/settings",                      HTTP_GET,  redirectToRoot);
  server.on("/settings/export",               HTTP_GET,  redirectToRoot);
  server.on("/settings/import",               HTTP_GET,  redirectToRoot);
  server.on("/settings/unlock",               HTTP_GET,  redirectToRoot);
  server.on("/settings/reset",                HTTP_GET,  redirectToRoot);
  server.on("/settings/factory-reset",        HTTP_GET,  redirectToRoot);
  server.on("/settings/read",                 HTTP_POST, handleSettingsReadPost);
  server.on("/settings",                      HTTP_POST, handleSettingsPost);
  server.on("/settings/export",               HTTP_POST, handleSettingsExportPost);
  server.on("/settings/import",               HTTP_POST, handleSettingsImportPost);
  server.on("/settings/unlock",               HTTP_POST, handleSettingsUnlockPost);
  server.on("/settings/reset",                HTTP_POST, handleSettingsRestoreDefaultsPost);
  server.on("/settings/factory-reset",        HTTP_POST, handleSettingsFactoryResetPost);
#endif
  server.on("/esp/reboot",                    HTTP_GET,  redirectToRoot);
  server.on("/esp/reboot",                    HTTP_POST, handleEspRebootPost);

  server.onNotFound([]() {
    String path = server.uri();

    // Common captive-portal probe endpoints used by phones/OSes.
    if (path == "/generate_204" ||
        path == "/gen_204" ||
        path == "/hotspot-detect.html" ||
        path == "/ncsi.txt" ||
        path == "/connecttest.txt" ||
        path == "/success.txt" ||
        path == "/redirect") {
      redirectToRoot();
      return;
    }

    if (path == "/") {
#if APP_MODE == APP_MODE_ESTOP
      path = "/estop.html";
#else
      path = "/index.html";
#endif
    }

    // Never expose the other app mode's UI pages even if assets exist in SPIFFS.
    if (isCrossModeUiAssetPath(path)) {
      redirectToRoot();
      return;
    }

    if (sendFileFromSPIFFS(path)) {
      return;
    }
    DAEMON_LOGF("HTTP request not found, redirecting to /: %s\n", path.c_str());
    redirectToRoot();
  });

  ElegantOTA.begin(&server);
  server.begin();
  const String mdnsHost = getDeviceNameForMdns();
  DAEMON_LOGLN("Web server started on port 80 (sync)");
  DAEMON_LOGF("OTA endpoint: http://%s.local/update\n", mdnsHost.c_str());
}

void handleWebServer() {
  server.handleClient();
  ElegantOTA.loop();
}
