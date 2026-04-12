/*
 * Matter WiFi WLED Bridge - WLED Device Discovery Implementation
 *
 * Uses ESP-IDF's native mDNS API (mdns_query_ptr) to find WLED devices
 * on the network via the _wled._tcp service type, then queries each
 * device's /json/info endpoint for detailed information.
 *
 * Architecture:
 *   - wledDiscoveryInit() is called once after WiFi connects. It calls
 *     mdns_init() which is safe to call even if CHIP has already init'd
 *     mDNS (returns ESP_OK as a no-op). It then calls mdns_netif_action()
 *     with MDNS_EVENT_ENABLE_IP4 to force the STA PCB to be enabled —
 *     this is necessary because Matter's WiFi takeover can cause a brief
 *     disconnect that leaves the mDNS PCB in PCB_OFF state.
 *   - wledDiscover() issues a PTR query for _wled._tcp with retry logic
 *     in case the PCB is still probing when the first query fires.
 *
 * WLED devices advertise as _wled._tcp (dedicated service type) with a
 * TXT record containing the MAC address.
 *
 * Uses ESP-IDF's esp_http_client (native, no Arduino SSL dependency).
 */

#include "wled_discovery.h"
#include "web_ui.h"
#include <mdns.h>
#include <ArduinoJson.h>
#include <esp_http_client.h>
#include <esp_netif.h>
#include <cstring>
#include <esp_log.h>

static const char* TAG = "Discovery";

// HTTP timeout for querying WLED device info (needs to be long enough for
// slow devices like HD panels with 4096+ LEDs, but short enough for good UX)
static const int DISCOVERY_HTTP_TIMEOUT_MS = 5000;

// Buffer for HTTP response body (WLED /json/info can be large on WLED-MM
// devices with matrix configs — typically 400-2000 bytes, up to ~4KB)
static const int RESPONSE_BUF_SIZE = 4096;

// Track whether we've initialised mDNS from this module
static bool sMdnsReady = false;

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
    ESP_LOGW(TAG, "JSON parse error from %s: %s", host, jsonErr.c_str());
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

// Helper: convert mdns_ip_protocol_t address to a string
static bool mdnsResultToIP(mdns_result_t* r, char* ipStr, size_t ipStrLen) {
  if (!r || !r->addr) return false;

  mdns_ip_addr_t* addr = r->addr;
  // Prefer IPv4
  while (addr) {
    if (addr->addr.type == ESP_IPADDR_TYPE_V4) {
      snprintf(ipStr, ipStrLen, IPSTR, IP2STR(&addr->addr.u_addr.ip4));
      return true;
    }
    addr = addr->next;
  }
  // Fall back to first address (IPv6)
  addr = r->addr;
  if (addr->addr.type == ESP_IPADDR_TYPE_V6) {
    snprintf(ipStr, ipStrLen, IPV6STR, IPV62STR(addr->addr.u_addr.ip6));
    return true;
  }
  return false;
}

void wledDiscoveryInit() {
  if (sMdnsReady) {
    return;
  }

  ESP_LOGI(TAG, "Initializing mDNS for WLED discovery...");

  // mdns_init() is safe to call if CHIP has already called it (returns ESP_OK
  // as a no-op). When CONFIG_MDNS_PREDEF_NETIF_STA=1, the init path:
  //   1. Creates the mDNS task (if not already running)
  //   2. Registers event handlers for WIFI_EVENT + IP_EVENT
  //   3. Scans all predefined interfaces for existing IPs and enables their PCBs
  esp_err_t err = mdns_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mdns_init() failed: %s", esp_err_to_name(err));
    return;
  }

  // After Matter starts and takes ownership of the WiFi interface, the mDNS
  // STA PCB may be left in PCB_OFF state due to a brief WiFi disconnect/
  // reconnect cycle during the handover.  Explicitly tell the mDNS stack to
  // (re-)enable the IPv4 PCB on the STA interface.  If the PCB is already
  // running this is a harmless re-probe; if it was stuck in PCB_OFF this
  // brings it back to life so mDNS queries actually work.
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (sta) {
    err = mdns_netif_action(sta, MDNS_EVENT_ENABLE_IP4);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "mdns_netif_action(ENABLE_IP4) failed: %s", esp_err_to_name(err));
    } else {
      ESP_LOGI(TAG, "mDNS STA PCB IPv4 enable requested");
    }
  } else {
    ESP_LOGW(TAG, "Could not get STA netif handle for mDNS PCB enable");
  }

  sMdnsReady = true;
  ESP_LOGI(TAG, "mDNS initialized for WLED discovery");
}

int wledDiscover(std::vector<WledDeviceInfo>& results, uint32_t timeoutMs) {
  results.clear();

  if (!isWiFiConnected()) {
    ESP_LOGW(TAG, "WiFi not connected, cannot discover WLED devices");
    return 0;
  }

  // Ensure mDNS is initialized (lazy init if wledDiscoveryInit wasn't called)
  if (!sMdnsReady) {
    wledDiscoveryInit();
    if (!sMdnsReady) {
      ESP_LOGE(TAG, "mDNS not available");
      return 0;
    }
    // Give the mDNS task a moment to start and enable PCBs
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  ESP_LOGI(TAG, "Scanning for WLED devices via mDNS (_wled._tcp)...");

  // Run multiple mDNS query passes and merge results. Low-power devices
  // (especially ESP8266-based WLED) may not respond to every mDNS query,
  // so a single pass often misses some. We run up to 3 passes with 4s each
  // and de-duplicate by hostname.
  static const int NUM_PASSES = 3;
  static const uint32_t PASS_TIMEOUT_MS = 4000;

  // Collect unique hostnames -> (ip, port) across all passes
  struct MdnsDevice {
    char hostname[64];
    char hostLocal[80];
    char ip[64];
    uint16_t port;
  };
  static const int MAX_DEVICES = 20;
  MdnsDevice found[MAX_DEVICES];
  int foundCount = 0;

  for (int pass = 0; pass < NUM_PASSES; pass++) {
    if (pass > 0) {
      vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGI(TAG, "mDNS query pass %d/%d, timeout: %lu ms", pass + 1, NUM_PASSES, (unsigned long)PASS_TIMEOUT_MS);

    mdns_result_t* mdnsResults = NULL;
    esp_err_t err = mdns_query_ptr("_wled", "_tcp", PASS_TIMEOUT_MS, 20, &mdnsResults);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "mDNS query failed: %s", esp_err_to_name(err));
      continue;
    }

    if (!mdnsResults) {
      ESP_LOGI(TAG, "mDNS pass %d found 0 WLED services", pass + 1);
      continue;
    }

    // Merge new results into found[]
    for (mdns_result_t* r = mdnsResults; r; r = r->next) {
      const char* hostname = r->hostname;
      if (!hostname) continue;

      // Check for duplicate
      bool dup = false;
      for (int j = 0; j < foundCount; j++) {
        if (strcasecmp(found[j].hostname, hostname) == 0) {
          dup = true;
          break;
        }
      }
      if (dup || foundCount >= MAX_DEVICES) continue;

      // Get IP address
      char ipStr[64];
      if (!mdnsResultToIP(r, ipStr, sizeof(ipStr))) continue;

      MdnsDevice& d = found[foundCount];
      strlcpy(d.hostname, hostname, sizeof(d.hostname));
      snprintf(d.hostLocal, sizeof(d.hostLocal), "%s.local", hostname);
      strlcpy(d.ip, ipStr, sizeof(d.ip));
      d.port = r->port;
      foundCount++;

      ESP_LOGI(TAG, "Pass %d: found WLED device: %s (%s:%d)", pass + 1, hostname, ipStr, d.port);
    }

    mdns_query_results_free(mdnsResults);
  }

  ESP_LOGI(TAG, "mDNS found %d unique WLED devices across %d passes", foundCount, NUM_PASSES);

  if (foundCount == 0) {
    return 0;
  }

  // Query each unique device for its WLED info
  for (int i = 0; i < foundCount; i++) {
    const MdnsDevice& d = found[i];

    ESP_LOGI(TAG, "Checking device: %s (%s / %s:%d)", d.hostname, d.hostLocal, d.ip, d.port);

    WledDeviceInfo info = {};
    strlcpy(info.host, d.ip, sizeof(info.host));
    strlcpy(info.hostname, d.hostLocal, sizeof(info.hostname));
    info.port = d.port;

    // Query using IP for speed (already resolved via mDNS)
    if (queryWledInfo(d.ip, d.port, info)) {
      ESP_LOGI(TAG, "Found WLED device: %s (%s / %s) - %d LEDs, RGBW=%d, v%s",
               info.name, info.hostname, info.host, info.ledCount, info.isRGBW, info.version);
      results.push_back(info);
    } else {
      ESP_LOGD(TAG, "Device %s is not a WLED device or not responding", d.hostname);
    }
  }

  ESP_LOGI(TAG, "Discovery complete: found %d WLED device(s)", (int)results.size());
  return results.size();
}
