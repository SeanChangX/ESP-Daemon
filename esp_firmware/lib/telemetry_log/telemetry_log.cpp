#include "telemetry_log.h"

#include <ArduinoJson.h>

namespace {

constexpr uint32_t kIntervalMs = 1000;
// Capacity cap for one battery session (avoid unbounded JSON export).
// Choose 2 hours at 1 Hz = 7200 samples.
constexpr size_t kCapacity = 2u * 60u * 60u;

float g_buf[kCapacity];
size_t g_count = 0;
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

// NOTE: telemetryLogGetJson() runs in the synchronous webserver task context.
// Avoid large stack allocations here (stack protection fault on ESP32-C3).
// We reuse this buffer when building the JSON response.
float s_copy[kCapacity];

bool g_connected_prev = false;
bool g_capturing = false;
bool g_truncated = false;
uint32_t g_session_start_ms = 0;
uint32_t g_last_session_uptime_sec = 0;

uint32_t g_last_push_ms = 0;

void clearSessionUnlocked(uint32_t nowMs) {
  g_count = 0;
  g_truncated = false;
  g_capturing = true;
  g_session_start_ms = nowMs;
  g_last_session_uptime_sec = 0;
  g_last_push_ms = 0; // allow immediate first push
}

}  // namespace

void telemetryLogInit() {
  portENTER_CRITICAL(&g_mux);
  g_count = 0;
  g_connected_prev = false;
  g_capturing = false;
  g_truncated = false;
  g_session_start_ms = 0;
  g_last_session_uptime_sec = 0;
  g_last_push_ms = 0;
  portEXIT_CRITICAL(&g_mux);
}

void telemetryLogMaybePush(float voltage_v, bool pack_connected) {
  const uint32_t now = millis();

  portENTER_CRITICAL(&g_mux);

  // Detect connect edge: start a new discharge session.
  if (pack_connected && !g_connected_prev) {
    clearSessionUnlocked(now);
  }

  // Detect disconnect edge: stop capturing but keep data.
  if (!pack_connected && g_connected_prev) {
    g_capturing = false;
    if (g_session_start_ms != 0) {
      g_last_session_uptime_sec = (now - g_session_start_ms) / 1000u;
    }
  }

  g_connected_prev = pack_connected;

  // Only capture while connected.
  if (!g_capturing) {
    portEXIT_CRITICAL(&g_mux);
    return;
  }

  // Rate limit to 1 Hz.
  if (g_last_push_ms != 0 && (uint32_t)(now - g_last_push_ms) < kIntervalMs) {
    portEXIT_CRITICAL(&g_mux);
    return;
  }
  g_last_push_ms = now;

  if (g_count < kCapacity) {
    g_buf[g_count++] = voltage_v;
  } else {
    // Stop pushing further to avoid overwriting; this makes JSON "session export" stable.
    g_truncated = true;
    g_capturing = false;
  }

  portEXIT_CRITICAL(&g_mux);
}

String telemetryLogGetJson() {
  size_t count = 0;
  uint32_t device_ms = 0;
  uint32_t uptime_sec = 0;
  uint32_t session_start_ms = 0;
  bool truncated = false;
  bool connected_now = false;

  portENTER_CRITICAL(&g_mux);
  count = g_count;
  device_ms = millis();
  truncated = g_truncated;
  session_start_ms = g_session_start_ms;
  connected_now = g_connected_prev;

  if (count > 0) {
    for (size_t i = 0; i < count; i++) {
      s_copy[i] = g_buf[i];
    }
    // Uptime is approximated by sample index (1 Hz).
    uptime_sec = (count - 1) * (kIntervalMs / 1000u);
  }

  // If we are still capturing, uptime should reflect current time.
  if (g_capturing && g_session_start_ms != 0) {
    uptime_sec = (device_ms - g_session_start_ms) / 1000u;
  } else if (!g_capturing) {
    // If we already disconnected, preserve the ended uptime.
    uptime_sec = g_last_session_uptime_sec;
  }
  portEXIT_CRITICAL(&g_mux);

  JsonDocument doc;
  doc["periodMs"]         = kIntervalMs;
  doc["capacity"]         = kCapacity;
  doc["count"]            = count;
  doc["deviceMs"]         = device_ms;
  doc["uptimeSec"]        = uptime_sec;
  doc["sessionStartMs"]   = session_start_ms;
  doc["truncated"]        = truncated;
  doc["connectedNow"]     = connected_now;

  JsonArray arr = doc["v"].to<JsonArray>();
  for (size_t i = 0; i < count; i++) {
    arr.add(s_copy[i]);
  }

  String out;
  serializeJson(doc, out);
  return out;
}
