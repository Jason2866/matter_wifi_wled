/*
 * Matter WiFi WLED Bridge - Main Entry Point
 *
 * ESP32-S3 firmware that:
 * 1. Provides a captive portal for WiFi setup
 * 2. Hosts a web UI for configuring WLED devices as Matter lights
 * 3. Presents each WLED device as a Matter Extended Color Light
 *    (compatible with Apple Home, Google Home, Amazon Alexa)
 * 4. Sends color/brightness commands to WLED devices via their JSON API
 *
 * Architecture:
 *   Apple Home / Google Home / Alexa
 *       |
 *   (Matter over WiFi)
 *       |
 *   ESP32-S3 --(HTTP JSON API)--> WLED devices
 *       |
 *   Web config UI
 *   for WLED device setup
 */

#include <Arduino.h>
#include <ArduinoOTA.h>
#include "config_store.h"
#include "wled_output.h"
#include "web_ui.h"
#include "matter_manager.h"
#include "log_buffer.h"

// WLED update rate: 2Hz
static const unsigned long WLED_UPDATE_INTERVAL_MS = 500;
static unsigned long lastWledUpdate = 0;

void setup() {
  Serial.begin(115200);
  logBufferInit();
  delay(1000);

  ESP_LOGI("Main", "=== Matter WiFi WLED Bridge ===");
  ESP_LOGI("Main", "Firmware v0.1.0");

  // 1. Load configuration from NVS
  configStore.begin();
  ESP_LOGI("Main", "Loaded %d light(s) from config", configStore.getLightCount());

  // 2. Init Matter node and endpoints (must be before WiFi for some SDK paths)
  matterSetup();

  // 3. Start WiFi + Web UI (this also triggers matterStart() on WiFi connect)
  webSetup();

  // 3b. Setup ArduinoOTA for network-based firmware upload from PlatformIO
  //     Disable ArduinoOTA's built-in mDNS — it conflicts with CHIP's minimal
  //     mDNS (both try to bind UDP port 5353).  OTA still works by IP address.
  ArduinoOTA.setHostname("matterwled");
  ArduinoOTA.setMdnsEnabled(false);
  ArduinoOTA.onStart([]() {
    ESP_LOGI("OTA", "OTA update starting...");
    wledOutput.stop();  // Stop WLED output during OTA
  });
  ArduinoOTA.onEnd([]() {
    ESP_LOGI("OTA", "OTA update complete, rebooting...");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    ESP_LOGE("OTA", "OTA error: %u", error);
  });
  ArduinoOTA.begin();

  // 4. Init WLED output
  wledOutput.begin();
}

// Interpolate a single channel value for smooth transitions
static uint8_t interpolate(uint8_t start, uint8_t end, float progress) {
  return static_cast<uint8_t>(start + (static_cast<float>(end) - static_cast<float>(start)) * progress);
}

void loop() {
  // Process web server tasks (DNS for captive portal, WiFi reconnect check)
  webLoop();

  // Handle OTA updates
  ArduinoOTA.handle();

  // Process Matter state changes
  matterLoop();

  // Update WLED devices at fixed rate (~2Hz)
  unsigned long now = millis();
  if (now - lastWledUpdate >= WLED_UPDATE_INTERVAL_MS) {
    lastWledUpdate = now;

    uint8_t count = configStore.getLightCount();
    if (count > 0) {
      // Build array of current states from Matter
      LightState states[MAX_LIGHTS];
      for (uint8_t i = 0; i < count; i++) {
        states[i] = matterGetLightState(i);

        // Apply transition interpolation if active
        if (states[i].transitioning) {
          if (now >= states[i].transitionEnd) {
            // Transition complete
            states[i].transitioning = false;
          } else {
            uint32_t elapsed = now - states[i].transitionStart;
            uint32_t duration = states[i].transitionEnd - states[i].transitionStart;
            float progress = (duration > 0) ? static_cast<float>(elapsed) / static_cast<float>(duration) : 1.0f;
            if (progress > 1.0f) progress = 1.0f;

            // Interpolate from start values toward target values
            states[i].brightness = interpolate(states[i].startBrightness, states[i].brightness, progress);
            states[i].red   = interpolate(states[i].startRed, states[i].red, progress);
            states[i].green = interpolate(states[i].startGreen, states[i].green, progress);
            states[i].blue  = interpolate(states[i].startBlue, states[i].blue, progress);
            states[i].white = interpolate(states[i].startWhite, states[i].white, progress);
          }
        }
      }

      // Update WLED devices
      wledOutput.update(&configStore.getLight(0), states, count);
    }
  }
}
