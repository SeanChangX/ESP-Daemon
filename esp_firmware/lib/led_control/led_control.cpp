#include "led_control.h"

#include "app_settings.h"

#include <Adafruit_NeoPixel.h>

namespace {

Adafruit_NeoPixel* g_strip = nullptr;

Adafruit_NeoPixel* stripOrNull() {
  return g_strip;
}

uint16_t ledCount() {
  return static_cast<uint16_t>(getAppSettings().led_count > 0 ? getAppSettings().led_count : 1);
}

uint8_t ledBrightness() {
  return getAppSettings().led_brightness;
}

} // namespace

// use volatile to ensure the variable is updated correctly in ISR
volatile int mode = 0;
volatile int sensor_mode = 0;
unsigned long last_override_time = 0;
unsigned long override_duration = 1000;
int current_mode;

void initLED() {
  const AppSettings& settings = getAppSettings();

  if (g_strip != nullptr) {
    delete g_strip;
    g_strip = nullptr;
  }

  g_strip = new Adafruit_NeoPixel(ledCount(), settings.led_pin, NEO_GRB + NEO_KHZ800);
  g_strip->begin();
  g_strip->show();
  g_strip->setBrightness(ledBrightness());

  override_duration = settings.led_override_duration_ms;
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

  static int pixelIndex = 0;
  static unsigned long lastUpdate = 0;
  static bool wipeOn = true;

  if (millis() - lastUpdate >= static_cast<unsigned long>(wait)) {
    strip->setBrightness(ledBrightness());
    if (wipeOn) {
      strip->setPixelColor(pixelIndex, color);
    } else {
      strip->setPixelColor(pixelIndex, 0);
    }
    strip->show();
    pixelIndex++;

    if (pixelIndex >= strip->numPixels()) {
      pixelIndex = 0;
      wipeOn = !wipeOn;
    }
    lastUpdate = millis();
  }
}

void breathingEffectNonBlocking(uint32_t color, int cycles) {
  Adafruit_NeoPixel* strip = stripOrNull();
  if (strip == nullptr) return;

  static int brightness = 0;
  static int direction = 5;
  static int cycleCount = 0;

  brightness += direction;

  if (brightness >= 255 || brightness <= 0) {
    direction = -direction;
    if (brightness <= 0) {
      cycleCount++;
      if (cycleCount >= cycles) {
        cycleCount = 0;
        brightness = 0;
        direction = 5;
        return;
      }
    }
  }

  strip->setBrightness(brightness);
  for (int i = 0; i < strip->numPixels(); i++) {
    strip->setPixelColor(i, color);
  }
  strip->show();
}

void rainbowNonBlocking(int wait) {
  Adafruit_NeoPixel* strip = stripOrNull();
  if (strip == nullptr) return;

  static long firstPixelHue = 0;
  static unsigned long lastUpdate = 0;

  if (millis() - lastUpdate >= static_cast<unsigned long>(wait)) {
    strip->setBrightness(ledBrightness());
    strip->rainbow(firstPixelHue);
    strip->show();
    firstPixelHue += 256;
    if (firstPixelHue >= 5 * 65536) {
      firstPixelHue = 0;
    }
    lastUpdate = millis();
  }
}

void LEDTask(void* pvParameters) {
  (void)pvParameters;

  for (;;) {
    if (mode == SIMA_CMD || mode == EME_ENABLE || mode == EME_DISABLE) {
      if (millis() - last_override_time > override_duration) {
        mode = -1;
      }
    }

    if (sensor_mode == BATT_LOW || sensor_mode == BATT_DISCONNECTED) {
      current_mode = sensor_mode;
    } else {
      current_mode = (mode == -1) ? sensor_mode : mode;
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
