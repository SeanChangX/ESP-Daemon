#include "battery_estimate.h"

#include <cmath>

namespace {

// Typical rest/open-circuit style window for rough SoC mapping (not coulomb-accurate).
constexpr float kMakita18FullV = 21.0f;
constexpr float kMakita18EmptyV = 15.5f;

// EMA on pack voltage (updated each sensor tick, ~10 Hz).
constexpr float kVoltageEmaAlpha = 0.15f;

// EMA on dV/dt (V/s), updated once per second from filtered voltage.
constexpr float kRateEmaBeta = 0.2f;

// Ignore time extrapolation unless voltage is falling at least this fast (V/s).
constexpr float kMinDischargeRate = 0.00012f;

float s_v_ema = 0.f;
float s_rate_ema = 0.f;
float s_v_at_tick = 0.f;
uint32_t s_last_tick_ms = 0;
bool s_ema_ready = false;
bool s_valid = false;

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void appendJsonLocked(JsonDocument& readings) {
  if (!s_ema_ready) {
    readings["estBattPct"] = -1;
    readings["estBattMin"] = -1;
    return;
  }

  const float span = kMakita18FullV - kMakita18EmptyV;
  float pct = 0.f;
  if (span > 0.001f) {
    pct = 100.f * (s_v_ema - kMakita18EmptyV) / span;
    if (pct < 0.f) {
      pct = 0.f;
    }
    if (pct > 100.f) {
      pct = 100.f;
    }
  }
  readings["estBattPct"] = static_cast<int>(lroundf(pct));

  int mins_remain = -1;
  // Only extrapolate remaining time while the pack is currently connected (avoid stale slope).
  if (s_valid && s_rate_ema < -kMinDischargeRate) {
    const float rem_v = s_v_ema - kMakita18EmptyV;
    if (rem_v <= 0.f) {
      mins_remain = 0;
    } else {
      const float t_sec = rem_v / (-s_rate_ema);
      if (t_sec > 0.f && t_sec < static_cast<float>(48 * 3600)) {
        mins_remain = static_cast<int>(lroundf(t_sec / 60.f));
        if (mins_remain < 0) {
          mins_remain = 0;
        }
      }
    }
  }
  readings["estBattMin"] = mins_remain;
}

}  // namespace

void batteryEstimateInit() {
  portENTER_CRITICAL(&s_mux);
  s_v_ema = 0.f;
  s_rate_ema = 0.f;
  s_v_at_tick = 0.f;
  s_last_tick_ms = 0;
  s_ema_ready = false;
  s_valid = false;
  portEXIT_CRITICAL(&s_mux);
}

void batteryEstimateUpdate(const float pack_volts, const bool pack_connected) {
  portENTER_CRITICAL(&s_mux);
  if (!pack_connected) {
    // Keep last filtered voltage so UI can still show % after disconnect.
    // Remaining-time extrapolation will be disabled because s_valid=false.
    s_valid = false;
    s_rate_ema = 0.f;
    s_last_tick_ms = 0;
    portEXIT_CRITICAL(&s_mux);
    return;
  }

  s_valid = true;
  const uint32_t now = millis();

  if (!s_ema_ready) {
    s_v_ema = pack_volts;
    s_ema_ready = true;
    s_last_tick_ms = now;
    s_v_at_tick = s_v_ema;
    s_rate_ema = 0.f;
    portEXIT_CRITICAL(&s_mux);
    return;
  }

  s_v_ema += kVoltageEmaAlpha * (pack_volts - s_v_ema);

  if (s_last_tick_ms == 0u) {
    s_last_tick_ms = now;
    s_v_at_tick = s_v_ema;
  } else if (static_cast<uint32_t>(now - s_last_tick_ms) >= 1000u) {
    const float dt_s = (now - s_last_tick_ms) / 1000.0f;
    if (dt_s > 0.2f) {
      const float inst_rate = (s_v_ema - s_v_at_tick) / dt_s;
      s_rate_ema += kRateEmaBeta * (inst_rate - s_rate_ema);
    }
    s_last_tick_ms = now;
    s_v_at_tick = s_v_ema;
  }

  portEXIT_CRITICAL(&s_mux);
}

void batteryEstimateAppendJson(JsonDocument& readings) {
  portENTER_CRITICAL(&s_mux);
  appendJsonLocked(readings);
  portEXIT_CRITICAL(&s_mux);
}
