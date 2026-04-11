/*
 * Matter WiFi WLED Bridge - WLED Output Implementation
 *
 * Sends HTTP POST requests to WLED devices' /json/state endpoint
 * to control color, brightness, and power state.
 *
 * Uses ESP-IDF's esp_http_client (native, no Arduino SSL dependency).
 */

#include "wled_output.h"
#include "web_ui.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_http_client.h>

WledOutput wledOutput;

// HTTP timeout in ms
static const int HTTP_TIMEOUT_MS = 10000;

// Consecutive error tracking per device
static uint32_t sendErrors[MAX_LIGHTS] = {};

void WledOutput::begin() {
  if (initialized) return;

  memset(lastSent, 0, sizeof(lastSent));
  memset(sendErrors, 0, sizeof(sendErrors));

  initialized = true;
  ESP_LOGI("WLED", "WLED output initialized");
}

bool WledOutput::stateChanged(uint8_t index, const LightState& state) const {
  if (index >= MAX_LIGHTS) return false;
  const SentState& last = lastSent[index];

  // Always send if we haven't sent yet
  if (!last.valid) return true;

  // Check for any change
  return (last.powerOn != state.powerOn ||
          last.brightness != state.brightness ||
          last.red != state.red ||
          last.green != state.green ||
          last.blue != state.blue ||
          last.white != state.white);
}

bool WledOutput::sendToWled(const LightConfig& cfg, const LightState& state) {
  if (!isWiFiConnected()) return false;
  if (cfg.wledHost[0] == '\0') return false;

  // Build the URL
  char url[128];
  if (cfg.wledPort != 80) {
    snprintf(url, sizeof(url), "http://%s:%u/json/state", cfg.wledHost, cfg.wledPort);
  } else {
    snprintf(url, sizeof(url), "http://%s/json/state", cfg.wledHost);
  }

  // Build JSON payload
  JsonDocument doc;

  if (!state.powerOn) {
    // Just turn off
    doc["on"] = false;
  } else {
    doc["on"] = true;

    // Map brightness (0-254) to WLED brightness (0-255)
    uint8_t wledBri = (state.brightness >= 254) ? 255
                    : static_cast<uint8_t>(state.brightness + (state.brightness > 0 ? 1 : 0));
    doc["bri"] = wledBri;

    // Build segment with solid effect and color
    JsonArray seg = doc["seg"].to<JsonArray>();
    JsonObject seg0 = seg.add<JsonObject>();
    seg0["fx"] = 0;  // Solid effect

    JsonArray col = seg0["col"].to<JsonArray>();
    JsonArray color0 = col.add<JsonArray>();
    color0.add(state.red);
    color0.add(state.green);
    color0.add(state.blue);

    // Add white channel for RGBW fixtures
    if (cfg.type == LIGHT_TYPE_RGBW) {
      color0.add(state.white);
    }
  }

  // Serialize
  char payload[256];
  size_t payloadLen = serializeJson(doc, payload, sizeof(payload));

  // Send HTTP POST using esp_http_client
  esp_http_client_config_t config = {};
  config.url = url;
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = HTTP_TIMEOUT_MS;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    ESP_LOGW("WLED", "HTTP client init failed for %s", cfg.wledHost);
    return false;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, payload, payloadLen);

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (err != ESP_OK) {
    ESP_LOGW("WLED", "HTTP POST to %s failed: %s", cfg.wledHost, esp_err_to_name(err));
    return false;
  }

  if (status == 200) {
    return true;
  } else {
    ESP_LOGW("WLED", "HTTP POST to %s returned %d", cfg.wledHost, status);
    return false;
  }
}

void WledOutput::update(const LightConfig* lights, const LightState* states, uint8_t count) {
  if (!initialized) return;
  if (!isWiFiConnected()) return;

  for (uint8_t i = 0; i < count; i++) {
    if (!lights[i].active) continue;
    if (lights[i].wledHost[0] == '\0') continue;

    // Only send if state changed (avoid redundant HTTP calls)
    if (!stateChanged(i, states[i])) continue;

    bool ok = sendToWled(lights[i], states[i]);

    if (ok) {
      // Record sent state
      lastSent[i].valid = true;
      lastSent[i].powerOn = states[i].powerOn;
      lastSent[i].brightness = states[i].brightness;
      lastSent[i].red = states[i].red;
      lastSent[i].green = states[i].green;
      lastSent[i].blue = states[i].blue;
      lastSent[i].white = states[i].white;

      if (sendErrors[i] > 0) {
        ESP_LOGI("WLED", "Device %s recovered after %lu errors",
                 lights[i].wledHost, sendErrors[i]);
      }
      sendErrors[i] = 0;
    } else {
      sendErrors[i]++;
      // Invalidate last-sent so we retry on next cycle
      lastSent[i].valid = false;
      // Rate-limit error logging: only log at powers of 2
      if ((sendErrors[i] & (sendErrors[i] - 1)) == 0) {
        ESP_LOGW("WLED", "Device %s: %lu consecutive send failures",
                 lights[i].wledHost, sendErrors[i]);
      }
    }
  }
}

void WledOutput::stop() {
  if (!initialized) return;

  memset(lastSent, 0, sizeof(lastSent));
  memset(sendErrors, 0, sizeof(sendErrors));

  initialized = false;
  ESP_LOGI("WLED", "WLED output stopped");
}
