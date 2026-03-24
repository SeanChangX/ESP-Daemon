#include "battery_estimate.h"

#include <cmath>

namespace {

// Typical rest/open-circuit style window for rough SoC mapping (not coulomb-accurate).
constexpr float kMakita18FullV = 21.0f;
constexpr float kMakita18EmptyV = 15.5f;

// EMA on pack voltage (updated each sensor tick, ~10 Hz). Lower alpha = steadier display.
constexpr float kVoltageEmaAlpha = 0.08f;

// Display smoothing for battery percentage (phone-like feel, avoid big jumps).
constexpr float kPercentEmaAlpha = 0.14f;

// Remaining-time model based on a long discharge window (stable, low jitter).
constexpr uint32_t kTimeWindowMinSec = 120u;     // Need at least 2 minutes data.
constexpr uint32_t kTimeWindowMaxSec = 900u;     // Re-anchor every 15 minutes.
constexpr float kTimeWindowMinDropV = 0.04f;     // Need enough voltage drop before extrapolation.
constexpr float kTimeWindowResetDropV = 0.35f;   // Re-anchor sooner on bigger drop.
constexpr float kMinDischargeRateVps = 0.00002f; // 0.072 V/hour floor.
constexpr float kTimeEstimateEmaAlpha = 0.18f;
constexpr uint32_t kTimeEstimateStaleMs = 120u * 60u * 1000u;
constexpr int kMaxRemainingMinutes = 48 * 60;

float s_v_ema = 0.f;
bool s_ema_ready = false;
bool s_pack_connected = false;

float s_pct_ema = 0.f;
bool s_pct_ready = false;

float s_window_anchor_v = 0.f;
uint32_t s_window_anchor_ms = 0;
float s_time_remaining_min_ema = 0.f;
bool s_time_ready = false;
uint32_t s_time_last_fresh_ms = 0;
uint32_t s_time_last_decay_ms = 0;

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

float clampf(const float v, const float lo, const float hi) {
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

float voltageToPct(const float volts) {
  const float span = kMakita18FullV - kMakita18EmptyV;
  if (span <= 0.001f) {
    return 0.f;
  }
  const float raw = 100.f * (volts - kMakita18EmptyV) / span;
  return clampf(raw, 0.f, 100.f);
}

void updatePercentLocked() {
  const float pct_raw = voltageToPct(s_v_ema);
  if (!s_pct_ready) {
    s_pct_ema = pct_raw;
    s_pct_ready = true;
    return;
  }

  // Fall a bit faster than rise to match user expectation under load.
  const float delta = pct_raw - s_pct_ema;
  const float alpha = (delta < 0.f) ? (kPercentEmaAlpha * 1.15f) : kPercentEmaAlpha;
  s_pct_ema += alpha * delta;
  s_pct_ema = clampf(s_pct_ema, 0.f, 100.f);
}

void updateTimeEstimateLocked(const uint32_t now_ms) {
  if (!s_pack_connected || !s_ema_ready) {
    s_time_ready = false;
    s_window_anchor_ms = 0;
    s_time_last_fresh_ms = 0;
    s_time_last_decay_ms = 0;
    return;
  }

  if (s_window_anchor_ms == 0u) {
    s_window_anchor_ms = now_ms;
    s_window_anchor_v = s_v_ema;
    return;
  }

  // Keep the anchor near the recent top if voltage rebounds.
  if (s_v_ema > s_window_anchor_v + 0.03f) {
    s_window_anchor_v = s_v_ema;
    s_window_anchor_ms = now_ms;
  }

  const uint32_t elapsed_ms = now_ms - s_window_anchor_ms;
  const float elapsed_sec = elapsed_ms / 1000.0f;
  const float drop_v = s_window_anchor_v - s_v_ema;
  bool fresh = false;

  if (elapsed_sec >= static_cast<float>(kTimeWindowMinSec) && drop_v >= kTimeWindowMinDropV) {
    const float rate_vps = drop_v / elapsed_sec;
    if (rate_vps >= kMinDischargeRateVps) {
      const float rem_v = s_v_ema - kMakita18EmptyV;
      float rem_min = 0.f;
      if (rem_v > 0.f) {
        rem_min = rem_v / rate_vps / 60.f;
      }
      rem_min = clampf(rem_min, 0.f, static_cast<float>(kMaxRemainingMinutes));

      if (!s_time_ready) {
        s_time_remaining_min_ema = rem_min;
        s_time_ready = true;
      } else {
        s_time_remaining_min_ema += kTimeEstimateEmaAlpha * (rem_min - s_time_remaining_min_ema);
      }
      s_time_remaining_min_ema = clampf(s_time_remaining_min_ema, 0.f, static_cast<float>(kMaxRemainingMinutes));
      s_time_last_fresh_ms = now_ms;
      s_time_last_decay_ms = now_ms;
      fresh = true;
    }
  }

  if (!fresh && s_time_ready) {
    // Keep countdown moving smoothly between fresh recalculations.
    if (s_time_last_decay_ms != 0u) {
      const float dt_min = (now_ms - s_time_last_decay_ms) / 60000.0f;
      if (dt_min > 0.01f) {
        s_time_remaining_min_ema = fmaxf(0.f, s_time_remaining_min_ema - dt_min);
        s_time_last_decay_ms = now_ms;
      }
    } else {
      s_time_last_decay_ms = now_ms;
    }

    if (s_time_last_fresh_ms != 0u && (now_ms - s_time_last_fresh_ms) > kTimeEstimateStaleMs) {
      s_time_ready = false;
      s_time_last_fresh_ms = 0;
    }
  }

  if (elapsed_sec >= static_cast<float>(kTimeWindowMaxSec) || drop_v >= kTimeWindowResetDropV) {
    s_window_anchor_ms = now_ms;
    s_window_anchor_v = s_v_ema;
  }
}

void appendJsonLocked(JsonDocument& readings) {
  if (!s_pct_ready) {
    readings["estBattPct"] = -1;
    readings["estBattMin"] = -1;
    return;
  }

  readings["estBattPct"] = static_cast<int>(lroundf(s_pct_ema));
  readings["estBattMin"] = (s_pack_connected && s_time_ready)
    ? static_cast<int>(lroundf(s_time_remaining_min_ema))
    : -1;
}

}  // namespace

void batteryEstimateInit() {
  portENTER_CRITICAL(&s_mux);
  s_v_ema = 0.f;
  s_ema_ready = false;
  s_pack_connected = false;
  s_pct_ema = 0.f;
  s_pct_ready = false;
  s_window_anchor_v = 0.f;
  s_window_anchor_ms = 0;
  s_time_remaining_min_ema = 0.f;
  s_time_ready = false;
  s_time_last_fresh_ms = 0;
  s_time_last_decay_ms = 0;
  portEXIT_CRITICAL(&s_mux);
}

void batteryEstimateUpdate(const float pack_volts, const bool pack_connected) {
  portENTER_CRITICAL(&s_mux);
  const uint32_t now = millis();

  if (!pack_connected) {
    // Keep last filtered voltage so UI can still show % after disconnect.
    // Remaining-time extrapolation is cleared because no active discharge session.
    s_pack_connected = false;
    s_time_ready = false;
    s_window_anchor_ms = 0;
    s_time_last_fresh_ms = 0;
    s_time_last_decay_ms = 0;
    portEXIT_CRITICAL(&s_mux);
    return;
  }

  s_pack_connected = true;

  if (!s_ema_ready) {
    s_v_ema = pack_volts;
    s_ema_ready = true;
    s_window_anchor_v = s_v_ema;
    s_window_anchor_ms = now;
    updatePercentLocked();
    portEXIT_CRITICAL(&s_mux);
    return;
  }

  s_v_ema += kVoltageEmaAlpha * (pack_volts - s_v_ema);
  updatePercentLocked();
  updateTimeEstimateLocked(now);

  portEXIT_CRITICAL(&s_mux);
}

void batteryEstimateAppendJson(JsonDocument& readings) {
  portENTER_CRITICAL(&s_mux);
  appendJsonLocked(readings);
  portEXIT_CRITICAL(&s_mux);
}
