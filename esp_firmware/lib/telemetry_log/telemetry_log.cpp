#include "telemetry_log.h"

#include <math.h>
#include <stdio.h>

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

size_t sanitizeMaxPoints(size_t max_points) {
  if (max_points < 2u) {
    return 2u;
  }
  return max_points;
}

size_t decimatedPointCount(size_t count, bool full, size_t max_points) {
  if (full) {
    return count;
  }
  const size_t maxPts = sanitizeMaxPoints(max_points);
  return (count > maxPts) ? maxPts : count;
}

size_t decimatedIndex(size_t out_idx, size_t out_count, size_t src_count) {
  if (src_count <= 1u || out_count <= 1u) {
    return 0u;
  }
  const uint64_t num = static_cast<uint64_t>(out_idx) * static_cast<uint64_t>(src_count - 1u);
  const uint64_t den = static_cast<uint64_t>(out_count - 1u);
  return static_cast<size_t>((num + (den / 2u)) / den);
}

void appendBoolJson(String& out, bool value) {
  out += (value ? "true" : "false");
}

void appendFloatJson(String& out, float value) {
  if (!isfinite(value)) {
    out += "null";
    return;
  }
  char buf[24];
  const int len = snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(value));
  if (len > 0) {
    out += buf;
  } else {
    out += "0";
  }
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

String telemetryLogGetJson(bool full, size_t max_points) {
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

  const size_t points = decimatedPointCount(count, full, max_points);
  const bool downsampled = (points < count);
  const size_t maxPointsUsed = sanitizeMaxPoints(max_points);

  // Reserve a practical lower bound to reduce heap churn on frequent HTTP polls.
  const size_t approxCharsPerPoint = 8u;
  String out;
  out.reserve(256u + (points * approxCharsPerPoint));

  out += "{";
  out += "\"periodMs\":";
  out += String(kIntervalMs);
  out += ",\"capacity\":";
  out += String(kCapacity);
  out += ",\"count\":";
  out += String(count);
  out += ",\"points\":";
  out += String(points);
  out += ",\"deviceMs\":";
  out += String(device_ms);
  out += ",\"uptimeSec\":";
  out += String(uptime_sec);
  out += ",\"sessionStartMs\":";
  out += String(session_start_ms);
  out += ",\"truncated\":";
  appendBoolJson(out, truncated);
  out += ",\"connectedNow\":";
  appendBoolJson(out, connected_now);
  out += ",\"downsampled\":";
  appendBoolJson(out, downsampled);
  if (!full) {
    out += ",\"maxPoints\":";
    out += String(maxPointsUsed);
  }
  out += ",\"v\":[";
  for (size_t i = 0; i < points; i++) {
    if (i > 0) {
      out += ",";
    }
    const size_t srcIndex = downsampled ? decimatedIndex(i, points, count) : i;
    appendFloatJson(out, s_copy[srcIndex]);
  }
  out += "]}";

  return out;
}
