/*
 * Matter WiFi WLED Bridge - WLED Device Discovery Implementation
 *
 * Uses ESP32 mDNS to find WLED devices on the network, then queries
 * each device's /json/info endpoint for detailed information.
 *
 * WLED devices advertise as _http._tcp. We identify them by checking
 * if the hostname starts with "wled" or by querying the /json/info endpoint.
 *
 * Uses ESP-IDF's esp_http_client (native, no Arduino SSL dependency).
 */

#include "wled_discovery.h"
#include "web_ui.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <esp_http_client.h>
#include <cstring>

// HTTP timeout for querying WLED device info
static const int DISCOVERY_HTTP_TIMEOUT_MS = 10000;

// Buffer for HTTP response body (WLED /json/info is typically ~400-800 bytes)
static const int RESPONSE_BUF_SIZE = 2048;

// HTTP event handler to collect response data
static esp_err_t httpEventHandler(esp_http_client_event_t* evt) {
  // We use the user_data field to point to a simple struct for buffering
  struct HttpBuf {
    char* buf;
    int   len;
    int   capacity;
  };

  HttpBuf* hb = static_cast<HttpBuf*>(evt->user_data);
  if (!hb) return ESP_OK;

  switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
      if (hb->len + evt->data_len < hb->capacity) {
        memcpy(hb->buf + hb->len, evt->data, evt->data_len);
        hb->len += evt->data_len;
        hb->buf[hb->len] = '\0';
      }
      break;
    default:
      break;
  }
  return ESP_OK;
}

// Query a single WLED device for its info
static bool queryWledInfo(const char* host, uint16_t port, WledDeviceInfo& info) {
  char url[128];
  if (port != 80) {
    snprintf(url, sizeof(url), "http://%s:%u/json/info", host, port);
  } else {
    snprintf(url, sizeof(url), "http://%s/json/info", host);
  }

  // Allocate response buffer on stack
  char responseBuf[RESPONSE_BUF_SIZE];
  responseBuf[0] = '\0';

  struct HttpBuf {
    char* buf;
    int   len;
    int   capacity;
  } httpBuf = { responseBuf, 0, RESPONSE_BUF_SIZE };

  esp_http_client_config_t config = {};
  config.url = url;
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = DISCOVERY_HTTP_TIMEOUT_MS;
  config.event_handler = httpEventHandler;
  config.user_data = &httpBuf;
  config.disable_auto_redirect = false;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    return false;
  }

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (err != ESP_OK || status != 200) {
    return false;
  }

  // Parse JSON response
  JsonDocument doc;
  DeserializationError jsonErr = deserializeJson(doc, responseBuf, httpBuf.len);
  if (jsonErr) {
    ESP_LOGW("Discovery", "JSON parse error from %s: %s", host, jsonErr.c_str());
    return false;
  }

  // Extract device info
  const char* name = doc["name"] | "WLED";
  strlcpy(info.name, name, sizeof(info.name));

  const char* mac = doc["mac"] | "";
  strlcpy(info.mac, mac, sizeof(info.mac));

  const char* ver = doc["ver"] | "";
  strlcpy(info.version, ver, sizeof(info.version));

  // LED info is under "leds" object
  JsonObjectConst leds = doc["leds"].as<JsonObjectConst>();
  if (!leds.isNull()) {
    info.ledCount = leds["count"] | 0;
    info.isRGBW = leds["rgbw"] | false;
  } else {
    info.ledCount = 0;
    info.isRGBW = false;
  }

  return true;
}

int wledDiscover(std::vector<WledDeviceInfo>& results, uint32_t timeoutMs) {
  results.clear();

  if (!isWiFiConnected()) {
    ESP_LOGW("Discovery", "WiFi not connected, cannot discover WLED devices");
    return 0;
  }

  // Initialize mDNS if not already running
  if (!MDNS.begin("matterwled")) {
    ESP_LOGW("Discovery", "mDNS init failed, retrying...");
    delay(100);
    if (!MDNS.begin("matterwled")) {
      ESP_LOGE("Discovery", "mDNS init failed twice, aborting discovery");
      return 0;
    }
  }

  ESP_LOGI("Discovery", "Scanning for WLED devices via mDNS...");

  // Query for _http._tcp services — WLED devices advertise as HTTP services
  int numServices = MDNS.queryService("http", "tcp");

  ESP_LOGI("Discovery", "mDNS found %d HTTP services", numServices);

  for (int i = 0; i < numServices; i++) {
    String hostname = MDNS.hostname(i);
    IPAddress ip = MDNS.address(i);
    uint16_t port = MDNS.port(i);

    // Filter for likely WLED devices by hostname
    String hostLower = hostname;
    hostLower.toLowerCase();

    // WLED hostnames typically start with "wled"
    bool likelyWled = hostLower.startsWith("wled");

    if (!likelyWled) {
      // Skip non-WLED devices to avoid slow HTTP timeouts
      continue;
    }

    String ipStr = ip.toString();

    ESP_LOGI("Discovery", "Checking device: %s (%s:%d)",
             hostname.c_str(), ipStr.c_str(), port);

    WledDeviceInfo info = {};
    strlcpy(info.host, ipStr.c_str(), sizeof(info.host));
    info.port = port;

    if (queryWledInfo(ipStr.c_str(), port, info)) {
      ESP_LOGI("Discovery", "Found WLED device: %s (%s) - %d LEDs, RGBW=%d, v%s",
               info.name, info.host, info.ledCount, info.isRGBW, info.version);
      results.push_back(info);
    } else {
      ESP_LOGD("Discovery", "Device %s is not a WLED device or not responding",
               hostname.c_str());
    }
  }

  ESP_LOGI("Discovery", "Discovery complete: found %d WLED device(s)", results.size());
  return results.size();
}
