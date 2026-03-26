#include "led_control.h"

#include "app_settings.h"
#include "voltmeter.h"

#include <Adafruit_NeoPixel.h>

namespace {

Adafruit_NeoPixel* g_strip          = nullptr;
uint16_t           g_led_count      = 1;
uint8_t            g_led_brightness = 200;
portMUX_TYPE       g_led_state_mux  = portMUX_INITIALIZER_UNLOCKED;
int                g_override_mode  = 0;
unsigned long      g_last_override_time = 0;
unsigned long      g_override_duration  = 1000;

struct ColorWipeAnimState {
  int pixel_index = 0;
  unsigned long last_update = 0;
  bool wipe_on = true;
};

struct BreathingAnimState {
  int brightness = 0;
  int direction = 5;
  int cycle_count = 0;
};

struct RainbowAnimState {
  long first_pixel_hue = 0;
  unsigned long last_update = 0;
};

ColorWipeAnimState g_color_wipe_state = {};
BreathingAnimState g_breathing_state = {};
RainbowAnimState g_rainbow_state = {};

Adafruit_NeoPixel* stripOrNull() {
  return g_strip;
}

uint16_t ledCount() {
  return g_led_count;
}

uint8_t ledBrightness() {
  return g_led_brightness;
}

void resetNonBlockingAnimations() {
  g_color_wipe_state = ColorWipeAnimState();
  g_breathing_state = BreathingAnimState();
  g_rainbow_state = RainbowAnimState();
  Adafruit_NeoPixel* strip = stripOrNull();
  if (strip != nullptr) {
    strip->setBrightness(ledBrightness());
  }
}

void getLedOverrideSnapshot(int& mode, unsigned long& last_override_time, unsigned long& override_duration) {
  portENTER_CRITICAL(&g_led_state_mux);
  mode = g_override_mode;
  last_override_time = g_last_override_time;
  override_duration = g_override_duration;
  portEXIT_CRITICAL(&g_led_state_mux);
}

} // namespace

void setLedOverrideState(int new_mode, unsigned long timestamp_ms) {
  portENTER_CRITICAL(&g_led_state_mux);
  g_override_mode = new_mode;
  g_last_override_time = timestamp_ms;
  portEXIT_CRITICAL(&g_led_state_mux);
}

int batteryStatusToLedMode(BatteryPackStatus status) {
  switch (status) {
    case BATTERY_STATUS_DISCONNECTED:
      return BATT_DISCONNECTED;
    case BATTERY_STATUS_LOW:
      return BATT_LOW;
    case BATTERY_STATUS_NORMAL:
    default:
      return DEFAULT_MODE;
  }
}

void initLED() {
  AppSettingsReadGuard settingsGuard;
  const AppSettings& settings = settingsGuard.settings();
  g_led_count = static_cast<uint16_t>(settings.led_count > 0 ? settings.led_count : 1);
  g_led_brightness = settings.led_brightness;

  if (g_strip != nullptr) {
    delete g_strip;
    g_strip = nullptr;
  }

  g_strip = new Adafruit_NeoPixel(g_led_count, settings.led_pin, NEO_GRB + NEO_KHZ800);
  g_strip->begin();
  g_strip->show();
  g_strip->setBrightness(ledBrightness());

  portENTER_CRITICAL(&g_led_state_mux);
  g_override_duration = settings.led_override_duration_ms;
  portEXIT_CRITICAL(&g_led_state_mux);
  resetNonBlockingAnimations();
}

void colorWipe(uint32_t color, int wait) {
  Adafruit_NeoPixel* strip = stripOrNull();
  if (strip == nullptr) return;

  for (int i = 0; i < strip->numPixels(); i++) {
    strip->setPixelColor(i, color);
    strip->show();
    delay(wait);
  }
}

void breathingEffect(uint32_t color, int cycles) {
  Adafruit_NeoPixel* strip = stripOrNull();
  if (strip == nullptr) return;

  for (int cycle = 0; cycle < cycles; cycle++) {
    for (int brightness = 0; brightness <= 250; brightness += 25) {
      strip->setBrightness(brightness);
      for (int i = 0; i < strip->numPixels(); i++) {
        strip->setPixelColor(i, color);
      }
      strip->show();
      delay(5);
    }
    for (int brightness = 250; brightness >= 0; brightness -= 25) {
      strip->setBrightness(brightness);
      for (int i = 0; i < strip->numPixels(); i++) {
        strip->setPixelColor(i, color);
      }
      strip->show();
      delay(5);
    }
  }
  strip->setBrightness(ledBrightness());
}

void rainbow(int wait) {
  Adafruit_NeoPixel* strip = stripOrNull();
  if (strip == nullptr) return;

  for (long firstPixelHue = 0; firstPixelHue < 5 * 65536; firstPixelHue += 256) {
    strip->rainbow(firstPixelHue);
    strip->show();
    delay(wait);
  }
}

void colorWipeNonBlocking(uint32_t color, int wait) {
  Adafruit_NeoPixel* strip = stripOrNull();
  if (strip == nullptr) return;

  if (millis() - g_color_wipe_state.last_update >= static_cast<unsigned long>(wait)) {
    strip->setBrightness(ledBrightness());
    if (g_color_wipe_state.wipe_on) {
      strip->setPixelColor(g_color_wipe_state.pixel_index, color);
    } else {
      strip->setPixelColor(g_color_wipe_state.pixel_index, 0);
    }
    strip->show();
    g_color_wipe_state.pixel_index++;

    if (g_color_wipe_state.pixel_index >= strip->numPixels()) {
      g_color_wipe_state.pixel_index = 0;
      g_color_wipe_state.wipe_on = !g_color_wipe_state.wipe_on;
    }
    g_color_wipe_state.last_update = millis();
  }
}

void breathingEffectNonBlocking(uint32_t color, int cycles) {
  Adafruit_NeoPixel* strip = stripOrNull();
  if (strip == nullptr) return;

  g_breathing_state.brightness += g_breathing_state.direction;

  if (g_breathing_state.brightness >= 255 || g_breathing_state.brightness <= 0) {
    g_breathing_state.direction = -g_breathing_state.direction;
    if (g_breathing_state.brightness <= 0) {
      g_breathing_state.cycle_count++;
      if (g_breathing_state.cycle_count >= cycles) {
        g_breathing_state.cycle_count = 0;
        g_breathing_state.brightness = 0;
        g_breathing_state.direction  = 5;
        return;
      }
    }
  }

  strip->setBrightness(g_breathing_state.brightness);
  for (int i = 0; i < strip->numPixels(); i++) {
    strip->setPixelColor(i, color);
  }
  strip->show();
}

void rainbowNonBlocking(int wait) {
  Adafruit_NeoPixel* strip = stripOrNull();
  if (strip == nullptr) return;

  if (millis() - g_rainbow_state.last_update >= static_cast<unsigned long>(wait)) {
    strip->setBrightness(ledBrightness());
    strip->rainbow(g_rainbow_state.first_pixel_hue);
    strip->show();
    g_rainbow_state.first_pixel_hue += 256;
    if (g_rainbow_state.first_pixel_hue >= 5 * 65536) {
      g_rainbow_state.first_pixel_hue = 0;
    }
    g_rainbow_state.last_update = millis();
  }
}

void LEDTask(void* pvParameters) {
  (void)pvParameters;
  int current_mode = DEFAULT_MODE;
  int previous_mode = -999;

  for (;;) {
    const int sensor_mode = batteryStatusToLedMode(getBatteryPackStatus());
    const unsigned long now = millis();
    int override_mode = 0;
    unsigned long last_override_time = 0;
    unsigned long override_duration = 0;
    getLedOverrideSnapshot(override_mode, last_override_time, override_duration);

    if (override_mode == SIMA_CMD || override_mode == EME_ENABLE || override_mode == EME_DISABLE) {
      if (now - last_override_time > override_duration) {
        setLedOverrideState(-1, last_override_time);
        override_mode = -1;
      }
    }

    if (sensor_mode == BATT_LOW || sensor_mode == BATT_DISCONNECTED) {
      current_mode = sensor_mode;
    } else {
      current_mode = (override_mode == -1) ? sensor_mode : override_mode;
    }

    if (current_mode != previous_mode) {
      resetNonBlockingAnimations();
      previous_mode = current_mode;
    }

    Adafruit_NeoPixel* strip = stripOrNull();
    if (strip != nullptr) {
      switch (current_mode) {
        case SIMA_CMD:
          breathingEffectNonBlocking(strip->Color(255, 255, 0), 5);
          break;
        case EME_DISABLE:
          breathingEffectNonBlocking(strip->Color(255, 0, 0), 5);
          break;
        case EME_ENABLE:
          breathingEffectNonBlocking(strip->Color(0, 255, 0), 5);
          break;
        case BATT_DISCONNECTED:
          colorWipeNonBlocking(strip->Color(0, 0, 255), 50);
          break;
        case BATT_LOW:
          colorWipeNonBlocking(strip->Color(255, 0, 0), 50);
          break;
        default:
          rainbowNonBlocking(5);
          break;
      }
    }

    vTaskDelay(1);
  }
}
