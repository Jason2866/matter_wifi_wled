/*
 * Matter WiFi WLED Bridge - Matter Manager Implementation
 *
 * Manages the Matter (Project CHIP) stack using a **Bridge architecture**:
 *   - Root node  (endpoint 0)   — basic device information
 *   - Aggregator (endpoint 1)   — bridge container (device type 0x000E)
 *   - Bridged nodes (endpoints 2+) — one per WLED device (device type 0x0013)
 *     each composited with Extended Color Light (device type 0x010D)
 *
 * Each bridged node has its own bridged_device_basic_information cluster
 * carrying a unique node_label so controllers (Google Home, Apple Home,
 * Alexa) display individual device names instead of a single "WLED Bridge".
 *
 * Receives on/off, brightness, and color commands from Matter controllers
 * and stores them in LightState structs for the WLED output module to consume.
 *
 * Based on the Matter over WiFi usermod from:
 * https://github.com/netmindz/WLED-MM/tree/matter-over-wifi/usermods/matter_over_wifi/
 */

#include "matter_manager.h"
#include "config_store.h"

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_system.h>

// ── Matter cluster IDs (from the Matter Application Cluster Specification) ──
static constexpr uint32_t MATTER_CL_ON_OFF       = 0x0006;
static constexpr uint32_t MATTER_CL_LEVEL_CTRL   = 0x0008;
static constexpr uint32_t MATTER_CL_COLOR_CTRL   = 0x0300;
static constexpr uint32_t MATTER_CL_BDBI         = 0x0039; // Bridged Device Basic Information

// ── Matter attribute IDs ────────────────────────────────────────────────────
// On/Off cluster
static constexpr uint32_t MATTER_AT_ON_OFF        = 0x0000;
// Level Control cluster
static constexpr uint32_t MATTER_AT_CURRENT_LEVEL = 0x0000;
// Color Control cluster
static constexpr uint32_t MATTER_AT_CURRENT_HUE   = 0x0000;
static constexpr uint32_t MATTER_AT_CURRENT_SAT   = 0x0001;
static constexpr uint32_t MATTER_AT_CURRENT_X      = 0x0003;
static constexpr uint32_t MATTER_AT_CURRENT_Y      = 0x0004;
static constexpr uint32_t MATTER_AT_COLOR_TEMP     = 0x0007;
static constexpr uint32_t MATTER_AT_COLOR_MODE     = 0x0008;

// ── Color mode values (Matter spec §3.2.7.10) ──────────────────────────────
static constexpr uint8_t MATTER_CM_HS   = 0; // Hue/Saturation
static constexpr uint8_t MATTER_CM_XY   = 1; // CIE x/y
static constexpr uint8_t MATTER_CM_TEMP = 2; // Color Temperature

// ── Module state ────────────────────────────────────────────────────────────
static esp_matter::node_t *mNode = nullptr;
static esp_matter::endpoint_t *mAggregator = nullptr; // Bridge aggregator endpoint
static bool mStarted     = false;   // true after esp_matter::start() succeeds
static bool mNodeReady   = false;   // true after node+endpoints created in setup
static bool mSuppressed  = false;   // true when no lights configured
static bool reconfigPending = false;

// Per-endpoint tracking
struct EndpointInfo {
  esp_matter::endpoint_t *endpoint;
  uint16_t endpointId;
};
static EndpointInfo endpoints[MAX_LIGHTS] = {};

// ── MAC-to-endpoint-ID registry ─────────────────────────────────────────────
// We persist a mapping of WLED device MAC → Matter endpoint ID in a dedicated
// NVS namespace ("mtwled_epmap").  On first boot for a given MAC, we use
// esp_matter::endpoint::bridged_node::create() which auto-assigns an ID, then
// save it.  On subsequent boots, we use bridged_node::resume() with the saved
// ID so that Matter controllers recognise the device as the same one.
//
// NVS key format: MAC with colons stripped, e.g. "AABBCCDDEEFF" (12 chars).
// NVS value: uint16_t endpoint ID.

static const char *EP_MAP_NS = "mtwled_epmap";

// Look up a previously-assigned endpoint ID for a MAC address.
// Returns the endpoint ID, or 0 if not found.
static uint16_t epMapLoad(const char *mac) {
  if (!mac || mac[0] == '\0') return 0;

  // Build NVS key: strip colons from MAC
  char key[13] = {};
  int k = 0;
  for (int i = 0; mac[i] != '\0' && k < 12; i++) {
    if (mac[i] != ':') key[k++] = mac[i];
  }
  key[k] = '\0';
  if (k == 0) return 0;

  nvs_handle_t h;
  if (nvs_open(EP_MAP_NS, NVS_READONLY, &h) != ESP_OK) return 0;
  uint16_t epId = 0;
  nvs_get_u16(h, key, &epId);
  nvs_close(h);
  return epId;
}

// Save an endpoint ID for a MAC address.
static void epMapSave(const char *mac, uint16_t epId) {
  if (!mac || mac[0] == '\0') return;

  char key[13] = {};
  int k = 0;
  for (int i = 0; mac[i] != '\0' && k < 12; i++) {
    if (mac[i] != ':') key[k++] = mac[i];
  }
  key[k] = '\0';
  if (k == 0) return;

  nvs_handle_t h;
  if (nvs_open(EP_MAP_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u16(h, key, epId);
  nvs_commit(h);
  nvs_close(h);
}

// Light states (written by Matter callbacks, read by main loop for WLED output)
static LightState lightStates[MAX_LIGHTS] = {};

// Per-light pending state from Matter callbacks (consumed in loop)
struct PendingState {
  volatile bool    onDirty;
  volatile bool    on;
  volatile bool    briDirty;
  volatile uint8_t bri;
  volatile bool    hsDirty;
  volatile uint8_t hue;
  volatile uint8_t sat;
  volatile bool    xyDirty;
  volatile uint16_t colorX;
  volatile uint16_t colorY;
  volatile bool    ctDirty;
  volatile uint16_t ct;
  volatile uint8_t colorMode;
};
static PendingState pendingStates[MAX_LIGHTS] = {};

// Syncing flag — suppresses re-entrant callbacks during WLED→Matter sync
static bool mSyncing = false;

// ── Commissioning configuration ─────────────────────────────────────────────
#ifndef MATTER_DEFAULT_PASSCODE
  #define MATTER_DEFAULT_PASSCODE 20202021
#endif
#ifndef MATTER_DEFAULT_DISCRIMINATOR
  #define MATTER_DEFAULT_DISCRIMINATOR 3840
#endif

static uint32_t mPasscode      = MATTER_DEFAULT_PASSCODE;
static uint16_t mDiscriminator = MATTER_DEFAULT_DISCRIMINATOR;

// Pending factory reset
static volatile bool mFactoryResetPending = false;

// ── Helpers: endpoint ID to light index ─────────────────────────────────────
static int endpointIdToIndex(uint16_t endpointId) {
  uint8_t count = configStore.getLightCount();
  for (uint8_t i = 0; i < count; i++) {
    if (endpoints[i].endpointId == endpointId) return i;
  }
  return -1;
}

// ── RGB-to-RGBW decomposition ───────────────────────────────────────────────
static void decomposeRGBW(int idx, uint8_t &r, uint8_t &g, uint8_t &b, uint8_t &w) {
  if (idx >= 0 && idx < configStore.getLightCount() &&
      configStore.getLight(idx).type == LIGHT_TYPE_RGBW) {
    w = min(r, min(g, b));
    r -= w;
    g -= w;
    b -= w;
  } else {
    w = 0;
  }
}

// ── HSV to RGB conversion ───────────────────────────────────────────────────
static void hsvToRGB(uint8_t matterHue, uint8_t matterSat,
                     uint8_t &r, uint8_t &g, uint8_t &b) {
  // Matter hue: 0-254 → 0-360 degrees
  // Matter sat: 0-254 → 0-1.0
  float h = (static_cast<float>(matterHue) / 254.0f) * 360.0f;
  float s = static_cast<float>(matterSat) / 254.0f;
  float v = 1.0f;  // brightness handled separately

  float c = v * s;
  float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
  float m = v - c;
  float rf, gf, bf;

  if (h < 60)       { rf = c; gf = x; bf = 0; }
  else if (h < 120) { rf = x; gf = c; bf = 0; }
  else if (h < 180) { rf = 0; gf = c; bf = x; }
  else if (h < 240) { rf = 0; gf = x; bf = c; }
  else if (h < 300) { rf = x; gf = 0; bf = c; }
  else              { rf = c; gf = 0; bf = x; }

  r = static_cast<uint8_t>((rf + m) * 255.0f);
  g = static_cast<uint8_t>((gf + m) * 255.0f);
  b = static_cast<uint8_t>((bf + m) * 255.0f);
}

// ── Color temperature (mirek) to RGB conversion ─────────────────────────────
static void mirekToRGB(uint16_t mirek, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (mirek < 153) mirek = 153;   // 6536K (cool daylight)
  if (mirek > 500) mirek = 500;   // 2000K (warm candle)

  float kelvin = 1000000.0f / static_cast<float>(mirek);
  float temp = kelvin / 100.0f;

  float rf, gf, bf;

  // Red
  if (temp <= 66.0f) {
    rf = 255.0f;
  } else {
    rf = 329.698727446f * powf(temp - 60.0f, -0.1332047592f);
  }

  // Green
  if (temp <= 66.0f) {
    gf = 99.4708025861f * logf(temp) - 161.1195681661f;
  } else {
    gf = 288.1221695283f * powf(temp - 60.0f, -0.0755148492f);
  }

  // Blue
  if (temp >= 66.0f) {
    bf = 255.0f;
  } else if (temp <= 19.0f) {
    bf = 0.0f;
  } else {
    bf = 138.5177312231f * logf(temp - 10.0f) - 305.0447927307f;
  }

  auto clamp = [](float v) -> float { return v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v); };
  rf = clamp(rf);
  gf = clamp(gf);
  bf = clamp(bf);

  // Normalize so max channel = 255
  float maxC = rf;
  if (gf > maxC) maxC = gf;
  if (bf > maxC) maxC = bf;
  if (maxC > 0.001f) {
    float scale = 255.0f / maxC;
    r = static_cast<uint8_t>(rf * scale + 0.5f);
    g = static_cast<uint8_t>(gf * scale + 0.5f);
    b = static_cast<uint8_t>(bf * scale + 0.5f);
  } else {
    r = g = b = 255;  // Fallback: white
  }
}

// ── CIE 1931 XY <-> RGB conversions ────────────────────────────────────────

// sRGB gamut triangle vertices in CIE 1931 xy
static constexpr float SRGB_RX = 0.6400f, SRGB_RY = 0.3300f;
static constexpr float SRGB_GX = 0.3000f, SRGB_GY = 0.6000f;
static constexpr float SRGB_BX = 0.1500f, SRGB_BY = 0.0600f;
static constexpr float D65_X   = 0.3127f, D65_Y   = 0.3290f;

// 2D cross product: (a - o) x (b - o)
static inline float cross2d(float ox, float oy,
                             float ax, float ay,
                             float bx, float by) {
  return (ax - ox) * (by - oy) - (ay - oy) * (bx - ox);
}

// Test if point (px,py) is inside the sRGB gamut triangle
static bool inSRGBGamut(float px, float py) {
  float d1 = cross2d(px, py, SRGB_RX, SRGB_RY, SRGB_GX, SRGB_GY);
  float d2 = cross2d(px, py, SRGB_GX, SRGB_GY, SRGB_BX, SRGB_BY);
  float d3 = cross2d(px, py, SRGB_BX, SRGB_BY, SRGB_RX, SRGB_RY);
  bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
  bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
  return !(hasNeg && hasPos);
}

// Intersect ray (p1 -> p2) with line segment (p3, p4).
// Returns parametric t along the ray, or -1 if no valid intersection.
static float raySegIntersect(float x1, float y1, float x2, float y2,
                              float x3, float y3, float x4, float y4) {
  float denom = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4);
  if (fabsf(denom) < 1e-10f) return -1.0f;
  float t = ((x1 - x3) * (y3 - y4) - (y1 - y3) * (x3 - x4)) / denom;
  float u = -((x1 - x2) * (y1 - y3) - (y1 - y2) * (x1 - x3)) / denom;
  return (u >= 0.0f && u <= 1.0f && t >= 0.0f) ? t : -1.0f;
}

// Map an out-of-gamut CIE xy point into the sRGB gamut by projecting from
// D65 white through the target point onto the gamut boundary. This preserves
// hue while clipping saturation — the standard approach for LED output.
static void mapToSRGBGamut(float &x, float &y) {
  if (inSRGBGamut(x, y)) return;

  // Edges of the sRGB triangle: R-G, G-B, B-R
  const float edges[][4] = {
    {SRGB_RX, SRGB_RY, SRGB_GX, SRGB_GY},
    {SRGB_GX, SRGB_GY, SRGB_BX, SRGB_BY},
    {SRGB_BX, SRGB_BY, SRGB_RX, SRGB_RY},
  };

  float bestT = -1.0f;
  float bestX = x, bestY = y;
  for (int i = 0; i < 3; i++) {
    float t = raySegIntersect(D65_X, D65_Y, x, y,
                               edges[i][0], edges[i][1],
                               edges[i][2], edges[i][3]);
    if (t > 0.0f && t <= 1.0f && t > bestT) {
      bestT = t;
      bestX = D65_X + t * (x - D65_X);
      bestY = D65_Y + t * (y - D65_Y);
    }
  }
  x = bestX;
  y = bestY;
}

static void xyToRGB(uint16_t zclX, uint16_t zclY,
                    uint8_t &r, uint8_t &g, uint8_t &b) {
  float x = static_cast<float>(zclX) / 65279.0f;
  float y = static_cast<float>(zclY) / 65279.0f;
  if (y < 0.001f) y = 0.001f;

  // Map out-of-gamut xy into sRGB triangle (controllers may use wider gamuts)
  mapToSRGBGamut(x, y);

  float Y = 1.0f;
  float X = (Y / y) * x;
  float Z = (Y / y) * (1.0f - x - y);

  float rf = X * 3.2404542f + Y * -1.5371385f + Z * -0.4985314f;
  float gf = X * -0.9692660f + Y * 1.8760108f + Z * 0.0415560f;
  float bf = X * 0.0556434f + Y * -0.2040259f + Z * 1.0572252f;

  // Clamp negative channels to 0 (out-of-gamut handling)
  if (rf < 0.0f) rf = 0.0f;
  if (gf < 0.0f) gf = 0.0f;
  if (bf < 0.0f) bf = 0.0f;

  // Normalize so max channel = 1.0 BEFORE gamma — preserves channel ratios
  // (clamping to [0,1] before gamma would destroy ratios for saturated colors)
  float maxC = rf;
  if (gf > maxC) maxC = gf;
  if (bf > maxC) maxC = bf;
  if (maxC > 0.001f) {
    rf /= maxC; gf /= maxC; bf /= maxC;
  }

  auto reverseGamma = [](float v) -> float {
    return v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
  };
  rf = reverseGamma(rf); gf = reverseGamma(gf); bf = reverseGamma(bf);

  r = static_cast<uint8_t>(rf * 255.0f + 0.5f);
  g = static_cast<uint8_t>(gf * 255.0f + 0.5f);
  b = static_cast<uint8_t>(bf * 255.0f + 0.5f);
}

static void rgbToXY(uint8_t r, uint8_t g, uint8_t b,
                    uint16_t &zclX, uint16_t &zclY) {
  // Apply sRGB gamma to get linear values
  auto applyGamma = [](float v) -> float {
    v /= 255.0f;
    return (v <= 0.04045f) ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
  };
  float rf = applyGamma(static_cast<float>(r));
  float gf = applyGamma(static_cast<float>(g));
  float bf = applyGamma(static_cast<float>(b));

  // sRGB to CIE XYZ (D65)
  float X = rf * 0.4124564f + gf * 0.3575761f + bf * 0.1804375f;
  float Y = rf * 0.2126729f + gf * 0.7151522f + bf * 0.0721750f;
  float Z = rf * 0.0193339f + gf * 0.1191920f + bf * 0.9503041f;

  float sum = X + Y + Z;
  if (sum < 0.0001f) {
    // Black — use D65 white point
    zclX = static_cast<uint16_t>(D65_X * 65279.0f + 0.5f);
    zclY = static_cast<uint16_t>(D65_Y * 65279.0f + 0.5f);
    return;
  }
  float cx = X / sum;
  float cy = Y / sum;
  zclX = static_cast<uint16_t>(cx * 65279.0f + 0.5f);
  zclY = static_cast<uint16_t>(cy * 65279.0f + 0.5f);
}

// ── RGB to Matter hue/saturation ────────────────────────────────────────────
static void rgbToMatterHS(uint8_t r, uint8_t g, uint8_t b,
                           uint8_t &matterHue, uint8_t &matterSat) {
  uint8_t maxC = max(r, max(g, b));
  uint8_t minC = min(r, min(g, b));
  matterSat = (maxC == 0) ? 0
            : (uint8_t)(((uint16_t)(maxC - minC) * 254U) / maxC);

  matterHue = 0;
  if (maxC != minC) {
    float delta = (float)(maxC - minC);
    float hf;
    if      (r == maxC) hf = fmodf((float)(g - b) / delta, 6.0f);
    else if (g == maxC) hf = ((float)(b - r) / delta) + 2.0f;
    else                hf = ((float)(r - g) / delta) + 4.0f;
    if (hf < 0.0f) hf += 6.0f;
    uint16_t raw = (uint16_t)(hf / 6.0f * 254.0f + 0.5f);
    matterHue = (raw > 254) ? 254 : (uint8_t)raw;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Matter SDK callbacks (invoked from the Matter task context)
// ─────────────────────────────────────────────────────────────────────────────

static esp_err_t _attrCb(esp_matter::attribute::callback_type_t type,
                          uint16_t endpoint_id,
                          uint32_t cluster_id,
                          uint32_t attribute_id,
                          esp_matter_attr_val_t *val,
                          void *priv_data)
{
  if (type != esp_matter::attribute::POST_UPDATE) return ESP_OK;
  if (mSyncing) return ESP_OK;

  int idx = endpointIdToIndex(endpoint_id);
  if (idx < 0) return ESP_OK;

  if (cluster_id == MATTER_CL_ON_OFF && attribute_id == MATTER_AT_ON_OFF) {
    pendingStates[idx].on      = val->val.b;
    pendingStates[idx].onDirty = true;
  } else if (cluster_id == MATTER_CL_LEVEL_CTRL && attribute_id == MATTER_AT_CURRENT_LEVEL) {
    pendingStates[idx].bri      = val->val.u8;
    pendingStates[idx].briDirty = true;
  } else if (cluster_id == MATTER_CL_COLOR_CTRL) {
    switch (attribute_id) {
      case MATTER_AT_CURRENT_HUE:
        pendingStates[idx].hue     = val->val.u8;
        pendingStates[idx].hsDirty = true;
        break;
      case MATTER_AT_CURRENT_SAT:
        pendingStates[idx].sat     = val->val.u8;
        pendingStates[idx].hsDirty = true;
        break;
      case MATTER_AT_CURRENT_X:
        pendingStates[idx].colorX  = val->val.u16;
        pendingStates[idx].xyDirty = true;
        break;
      case MATTER_AT_CURRENT_Y:
        pendingStates[idx].colorY  = val->val.u16;
        pendingStates[idx].xyDirty = true;
        break;
      case MATTER_AT_COLOR_TEMP:
        pendingStates[idx].ct      = val->val.u16;
        pendingStates[idx].ctDirty = true;
        break;
      case MATTER_AT_COLOR_MODE:
        pendingStates[idx].colorMode = val->val.u8;
        break;
    }
  }
  return ESP_OK;
}

static esp_err_t _identifyCb(esp_matter::identification::callback_type_t type,
                              uint16_t endpoint_id,
                              uint8_t  effect_id,
                              uint8_t  effect_variant,
                              void    *priv_data)
{
  // Identification request — could flash WLED devices; ignored for now.
  return ESP_OK;
}

static void _eventCb(const ChipDeviceEvent *event, intptr_t arg)
{
  // Reserved for handling commissioning / fabric events in the future.
}

// ─────────────────────────────────────────────────────────────────────────────
// Apply pending Matter state to light states (called from Arduino loop context)
// ─────────────────────────────────────────────────────────────────────────────

static void applyPending() {
  uint8_t count = configStore.getLightCount();

  for (uint8_t i = 0; i < count; i++) {
    PendingState &p = pendingStates[i];

    bool anyChange = p.onDirty || p.briDirty || p.hsDirty || p.xyDirty || p.ctDirty;
    if (!anyChange) continue;

    // Snapshot and clear flags
    bool applyOn  = p.onDirty;  p.onDirty  = false;
    bool applyBri = p.briDirty; p.briDirty = false;
    bool applyHS  = p.hsDirty;  p.hsDirty  = false;
    bool applyXY  = p.xyDirty;  p.xyDirty  = false;
    bool applyCT  = p.ctDirty;  p.ctDirty  = false;

    if (applyOn || applyBri) {
      bool on = p.on;
      uint8_t level = p.bri;
      if (on) {
        lightStates[i].powerOn = true;
        lightStates[i].brightness = (level > 0) ? level : 128;
      } else {
        lightStates[i].powerOn = false;
        lightStates[i].brightness = 0;
      }
    }

    if (applyCT && p.colorMode == MATTER_CM_TEMP) {
      uint8_t r, g, b, w;
      mirekToRGB(p.ct, r, g, b);
      decomposeRGBW(i, r, g, b, w);
      lightStates[i].red = r;
      lightStates[i].green = g;
      lightStates[i].blue = b;
      lightStates[i].white = w;
      uint16_t zclX, zclY;
      rgbToXY(r, g, b, zclX, zclY);
      lightStates[i].colorX = static_cast<float>(zclX) / 65279.0f;
      lightStates[i].colorY = static_cast<float>(zclY) / 65279.0f;
    } else if (applyXY && p.colorMode == MATTER_CM_XY) {
      uint8_t r, g, b, w;
      xyToRGB(p.colorX, p.colorY, r, g, b);
      decomposeRGBW(i, r, g, b, w);
      lightStates[i].red = r;
      lightStates[i].green = g;
      lightStates[i].blue = b;
      lightStates[i].white = w;
      lightStates[i].colorX = static_cast<float>(p.colorX) / 65279.0f;
      lightStates[i].colorY = static_cast<float>(p.colorY) / 65279.0f;
    } else if (applyHS && p.colorMode != MATTER_CM_TEMP) {
      uint8_t r, g, b, w;
      hsvToRGB(p.hue, p.sat, r, g, b);
      decomposeRGBW(i, r, g, b, w);
      lightStates[i].red = r;
      lightStates[i].green = g;
      lightStates[i].blue = b;
      lightStates[i].white = w;
      uint16_t zclX, zclY;
      rgbToXY(r, g, b, zclX, zclY);
      lightStates[i].colorX = static_cast<float>(zclX) / 65279.0f;
      lightStates[i].colorY = static_cast<float>(zclY) / 65279.0f;
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Push WLED state changes (from web UI or external) back to Matter
// ─────────────────────────────────────────────────────────────────────────────

static byte prevCol[MAX_LIGHTS][4] = {};
static byte prevBri[MAX_LIGHTS] = {};
static bool prevOn[MAX_LIGHTS] = {};

static void syncToMatter() {
  uint8_t count = configStore.getLightCount();
  mSyncing = true;

  for (uint8_t i = 0; i < count; i++) {
    bool curOn = lightStates[i].powerOn;
    uint8_t curBri = lightStates[i].brightness;
    uint8_t curR = lightStates[i].red;
    uint8_t curG = lightStates[i].green;
    uint8_t curB = lightStates[i].blue;

    if (curOn == prevOn[i] && curBri == prevBri[i] &&
        curR == prevCol[i][0] && curG == prevCol[i][1] &&
        curB == prevCol[i][2])
      continue;

    prevOn[i] = curOn;
    prevBri[i] = curBri;
    prevCol[i][0] = curR;
    prevCol[i][1] = curG;
    prevCol[i][2] = curB;

    uint16_t epId = endpoints[i].endpointId;
    esp_matter_attr_val_t val;

    // On/Off
    val = esp_matter_bool(curOn);
    esp_matter::attribute::update(epId, MATTER_CL_ON_OFF,
                                  MATTER_AT_ON_OFF, &val);

    // Level — only update when on.  The Matter Level Control cluster has a
    // minimum value of 1; pushing 0 causes error 0x87 (CONSTRAINT_ERROR).
    // When the light is off, the on_off attribute already reflects the state
    // and current_level retains its last-known brightness (per Matter spec).
    if (curOn && curBri > 0) {
      val = esp_matter_nullable_uint8(curBri);
      esp_matter::attribute::update(epId, MATTER_CL_LEVEL_CTRL,
                                    MATTER_AT_CURRENT_LEVEL, &val);
    }

    // Color — convert RGB to Matter hue/saturation
    uint8_t matterHue, matterSat;
    rgbToMatterHS(curR, curG, curB, matterHue, matterSat);

    val = esp_matter_nullable_uint8(matterHue);
    esp_matter::attribute::update(epId, MATTER_CL_COLOR_CTRL,
                                  MATTER_AT_CURRENT_HUE, &val);

    val = esp_matter_nullable_uint8(matterSat);
    esp_matter::attribute::update(epId, MATTER_CL_COLOR_CTRL,
                                  MATTER_AT_CURRENT_SAT, &val);
  }

  mSyncing = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// QR code / pairing code generation (Matter Core Spec §5.1.3 / §5.1.4)
// ─────────────────────────────────────────────────────────────────────────────

static void buildQRPayload(char *out, size_t outLen) {
  static const char kBase38[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-.";

  const uint16_t vendor_id   = 0xFFF1;
  const uint16_t product_id  = 0x8000;
  const uint8_t  flow        = 0;
  const uint16_t rendezvous  = 4; // OnNetwork

  uint8_t buf[12] = {};
  uint64_t lo = 0, hi = 0;
  lo |= ((uint64_t)(vendor_id)  & 0xFFFF) << 3;
  lo |= ((uint64_t)(product_id) & 0xFFFF) << 19;
  lo |= ((uint64_t)(flow)       & 0x3)    << 35;
  lo |= ((uint64_t)(rendezvous) & 0xFFF)  << 37;
  lo |= ((uint64_t)(mDiscriminator & 0xFFF)) << 49;
  lo |= ((uint64_t)(mPasscode & 0x7)) << 61;
  hi  = (uint64_t)(mPasscode >> 3) & 0xFFFFFF;
  for (int i = 0; i < 8; i++) buf[i] = (lo >> (8*i)) & 0xFF;
  for (int i = 0; i < 3; i++) buf[8+i] = (hi >> (8*i)) & 0xFF;

  char encoded[20] = {};
  int pos = 0;
  for (int i = 0; i < 9; i += 3) {
    uint32_t val = buf[i] | ((uint32_t)buf[i+1] << 8) | ((uint32_t)buf[i+2] << 16);
    for (int j = 0; j < 5; j++) { encoded[pos++] = kBase38[val % 38]; val /= 38; }
  }
  uint32_t val = buf[9] | ((uint32_t)buf[10] << 8);
  for (int j = 0; j < 3; j++) { encoded[pos++] = kBase38[val % 38]; val /= 38; }
  encoded[pos] = '\0';

  snprintf(out, outLen, "MT:%s", encoded);
}

static void buildManualCode(char *out, size_t outLen) {
  uint32_t chunk1 = (mDiscriminator >> 10) & 0x3;
  uint32_t chunk2 = ((mDiscriminator & 0x300) << 6) | (mPasscode & 0x3FFF);
  uint32_t chunk3 = (mPasscode >> 14) & 0x1FFF;

  char digits[11];
  snprintf(digits, sizeof(digits), "%01lu%05lu%04lu",
           (unsigned long)chunk1, (unsigned long)chunk2, (unsigned long)chunk3);

  // Verhoeff check digit
  static const uint8_t kD[10][10] = {
    {0,1,2,3,4,5,6,7,8,9},{1,2,3,4,0,6,7,8,9,5},{2,3,4,0,1,7,8,9,5,6},
    {3,4,0,1,2,8,9,5,6,7},{4,0,1,2,3,9,5,6,7,8},{5,9,8,7,6,0,4,3,2,1},
    {6,5,9,8,7,1,0,4,3,2},{7,6,5,9,8,2,1,0,4,3},{8,7,6,5,9,3,2,1,0,4},
    {9,8,7,6,5,4,3,2,1,0},
  };
  static const uint8_t kP[8][10] = {
    {0,1,2,3,4,5,6,7,8,9},{1,5,7,6,2,8,3,0,9,4},{5,8,0,3,7,9,6,1,4,2},
    {8,9,1,6,0,4,3,5,2,7},{9,4,5,3,1,2,6,8,7,0},{4,2,8,6,5,7,3,9,0,1},
    {2,7,9,3,8,0,6,4,1,5},{7,0,4,6,9,1,3,2,5,8},
  };
  static const uint8_t kInv[10] = {0,4,3,2,1,5,6,7,8,9};

  uint8_t c = 0;
  for (int i = 9; i >= 0; i--) {
    int p = 10 - 1 - i;
    c = kD[c][kP[(p + 1) % 8][digits[i] - '0']];
  }
  uint8_t check = kInv[c];

  snprintf(out, outLen, "%s%u", digits, check);
}

// ─────────────────────────────────────────────────────────────────────────────
// Matter-only NVS factory reset
// ─────────────────────────────────────────────────────────────────────────────

static void doMatterFactoryReset() {
  static const char * const kNamespaces[] = {
    "chip-config",
    "chip-counters",
    "chip-factory",
    "CHIP_KVS",
    "esp_matter_kvs",
    "node",
    "mtwled_epmap",  // MAC→endpoint ID registry
  };

  ESP_LOGI("Matter", "Factory reset — erasing Matter NVS namespaces");

  for (const char *ns : kNamespaces) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
      nvs_erase_all(handle);
      nvs_commit(handle);
      nvs_close(handle);
      ESP_LOGI("Matter", "Cleared namespace '%s'", ns);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGD("Matter", "Namespace '%s' not found (skipping)", ns);
    } else {
      ESP_LOGW("Matter", "Failed to open namespace '%s' (0x%x)", ns, err);
    }
  }

  ESP_LOGI("Matter", "Restarting...");
  esp_restart();
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void matterSetup() {
  uint8_t count = configStore.getLightCount();

  // If no lights configured, suppress Matter entirely.
  if (count == 0) {
    mSuppressed = true;
    ESP_LOGW("Matter", "No lights configured — Matter disabled. Add lights via web UI and restart.");
    return;
  }

  // Write product/vendor name to NVS "chip-factory" namespace so the
  // DeviceInstanceInfoProvider returns our name instead of "TEST_PRODUCT".
  // These are read by ESP32FactoryDataProvider::GetProductName/GetVendorName.
  {
    nvs_handle_t h;
    if (nvs_open("chip-factory", NVS_READWRITE, &h) == ESP_OK) {
      nvs_set_str(h, "vendor-name", "Matter WLED");
      nvs_set_str(h, "product-name", "WLED Bridge");
      nvs_commit(h);
      nvs_close(h);
    }
  }

  // Initialize default light states
  for (uint8_t i = 0; i < count; i++) {
    lightStates[i].powerOn = true;
    lightStates[i].brightness = 254;
    uint8_t r = 255, g = 255, b = 255, w;
    decomposeRGBW(i, r, g, b, w);
    lightStates[i].red = r;
    lightStates[i].green = g;
    lightStates[i].blue = b;
    lightStates[i].white = w;
    lightStates[i].colorX = D65_X;
    lightStates[i].colorY = D65_Y;
    lightStates[i].transitioning = false;

    memset(&pendingStates[i], 0, sizeof(PendingState));
  }

  // Create Matter node — set the node_label so controllers (Google Home, etc.)
  // show a meaningful name instead of "TEST_PRODUCT" or "Matter Device".
  esp_matter::node::config_t nodeCfg;
  strncpy(nodeCfg.root_node.basic_information.node_label, "Matter WLED Bridge", 32);
  mNode = esp_matter::node::create(&nodeCfg, _attrCb, _identifyCb);
  if (!mNode) {
    ESP_LOGE("Matter", "Node creation failed");
    return;
  }

  // ── Bridge architecture ──────────────────────────────────────────────────
  // 1. Create an Aggregator endpoint (device type 0x000E) to serve as the
  //    bridge container. All bridged nodes become children of this endpoint.
  esp_matter::endpoint::aggregator::config_t aggCfg;
  mAggregator = esp_matter::endpoint::aggregator::create(
      mNode, &aggCfg, esp_matter::ENDPOINT_FLAG_NONE, nullptr);
  if (!mAggregator) {
    ESP_LOGE("Matter", "Aggregator endpoint creation failed");
    return;
  }
  uint16_t aggId = esp_matter::endpoint::get_id(mAggregator);
  ESP_LOGI("Matter", "Aggregator endpoint created (id=%d)", aggId);

  // 2. For each configured light, create a Bridged Node endpoint (device type
  //    0x0013) with an Extended Color Light composited on top.  Each bridged
  //    node carries its own bridged_device_basic_information cluster with a
  //    unique node_label — this is what gives each device its own name in
  //    Google Home, Apple Home, and Alexa.
  //
  //    If the WLED device has a known MAC with a previously-assigned endpoint
  //    ID, we use resume() to re-create at the same ID.  Otherwise we use
  //    create() and save the newly-assigned ID for next time.
  for (uint8_t i = 0; i < count; i++) {
    const LightConfig &cfg = configStore.getLight(i);

    // 2a. Create or resume the bridged_node endpoint
    esp_matter::endpoint::bridged_node::config_t bridgedCfg;
    bridgedCfg.bridged_device_basic_information.reachable = true;

    esp_matter::endpoint_t *ep = nullptr;
    uint16_t savedEpId = epMapLoad(cfg.mac);

    if (savedEpId != 0) {
      // Known device — resume at the same endpoint ID
      ep = esp_matter::endpoint::bridged_node::resume(
          mNode, &bridgedCfg,
          esp_matter::ENDPOINT_FLAG_BRIDGE,
          savedEpId, nullptr);
      if (ep) {
        ESP_LOGI("Matter", "Resumed endpoint %d for MAC %s ('%s')",
                 savedEpId, cfg.mac, cfg.name);
      } else {
        // resume() can fail if min_unused_endpoint_id was reset (e.g. after
        // a Matter factory reset).  Fall through to create() below.
        ESP_LOGW("Matter", "resume() failed for MAC %s ep=%d, falling back to create()",
                 cfg.mac, savedEpId);
      }
    }

    if (!ep) {
      // New device (or resume failed) — create with auto-assigned ID
      ep = esp_matter::endpoint::bridged_node::create(
          mNode, &bridgedCfg,
          esp_matter::ENDPOINT_FLAG_BRIDGE,
          nullptr);
      if (!ep) {
        ESP_LOGE("Matter", "Bridged node %d creation failed", i);
        continue;
      }

      // Save the newly-assigned endpoint ID keyed by MAC (if MAC is known)
      uint16_t newEpId = esp_matter::endpoint::get_id(ep);
      if (cfg.mac[0] != '\0') {
        epMapSave(cfg.mac, newEpId);
        ESP_LOGI("Matter", "Created endpoint %d for MAC %s ('%s') — saved to registry",
                 newEpId, cfg.mac, cfg.name);
      } else {
        ESP_LOGW("Matter", "Created endpoint %d for '%s' — no MAC, cannot persist ID",
                 newEpId, cfg.name);
      }
    }

    // 2b. Set the aggregator as parent so the parts_list is populated correctly
    esp_matter::endpoint::set_parent_endpoint(ep, mAggregator);

    // 2c. Composite the Extended Color Light device type onto the bridged node
    esp_matter::endpoint::extended_color_light::config_t lightCfg;
    lightCfg.on_off.on_off                     = true;
    lightCfg.level_control.current_level       = 254;
    lightCfg.color_control.color_mode          = MATTER_CM_HS;
    lightCfg.color_control.enhanced_color_mode = MATTER_CM_HS;
    lightCfg.color_control.hue_saturation.current_hue        = 0;
    lightCfg.color_control.hue_saturation.current_saturation = 0;

    esp_err_t err = esp_matter::endpoint::extended_color_light::add(ep, &lightCfg);
    if (err != ESP_OK) {
      ESP_LOGE("Matter", "Failed to add extended_color_light to bridged node %d (0x%x)", i, err);
      continue;
    }

    // 2d. Add hue_saturation and xy features so color_capabilities bits are set
    //     and controllers expose both HS and CIE xy colour controls.
    esp_matter::cluster_t *cc = esp_matter::cluster::get(ep, MATTER_CL_COLOR_CTRL);
    if (cc) {
      esp_matter::cluster::color_control::feature::hue_saturation::config_t hsCfg;
      hsCfg.current_hue        = 0;
      hsCfg.current_saturation = 0;
      esp_matter::cluster::color_control::feature::hue_saturation::add(cc, &hsCfg);

      esp_matter::cluster::color_control::feature::xy::config_t xyCfg;
      xyCfg.current_x = static_cast<uint16_t>(D65_X * 65279.0f + 0.5f);
      xyCfg.current_y = static_cast<uint16_t>(D65_Y * 65279.0f + 0.5f);
      esp_matter::cluster::color_control::feature::xy::add(cc, &xyCfg);
    }

    // 2e. Add per-endpoint name attributes to bridged_device_basic_information
    //     cluster (0x0039). The cluster was created by bridged_node::create()
    //     but only has feature_map, unique_id, cluster_revision, and reachable.
    //     We must manually add node_label, product_name, and unique_id.
    esp_matter::cluster_t *bdbi = esp_matter::cluster::get(ep, MATTER_CL_BDBI);
    if (bdbi) {
      // node_label — the primary display name controllers use
      char label[33] = {};
      strncpy(label, cfg.name, 32);
      esp_matter::cluster::bridged_device_basic_information::attribute::create_node_label(
          bdbi, label, strlen(label));

      // product_name — secondary descriptor, e.g. "WLED Light"
      char prodName[] = "WLED Light";
      esp_matter::cluster::bridged_device_basic_information::attribute::create_product_name(
          bdbi, prodName, strlen(prodName));

      // unique_id — must be unique per bridged device for controller dedup.
      //             Use MAC address if available (stable across reboots and
      //             index changes), otherwise fall back to "wled-<index>-<name>".
      char uid[33] = {};
      if (cfg.mac[0] != '\0') {
        snprintf(uid, sizeof(uid), "wled-%s", cfg.mac);
      } else {
        snprintf(uid, sizeof(uid), "wled-%d-%.24s", i, cfg.name);
      }
      esp_matter::cluster::bridged_device_basic_information::attribute::create_unique_id(
          bdbi, uid, strlen(uid));
    } else {
      ESP_LOGW("Matter", "Bridged node %d: bridged_device_basic_information cluster not found", i);
    }

    endpoints[i].endpoint = ep;
    endpoints[i].endpointId = esp_matter::endpoint::get_id(ep);

    ESP_LOGI("Matter", "Created bridged endpoint %d (id=%d) for light '%s'",
             i, endpoints[i].endpointId, cfg.name);
  }

  mNodeReady = true;
  ESP_LOGI("Matter", "Node ready with %d endpoint(s)", count);
}

void matterStart() {
  if (!mNodeReady || mStarted) return;

  ESP_LOGI("Matter", "Starting Matter stack");
  esp_err_t err = esp_matter::start(_eventCb);
  if (err != ESP_OK) {
    ESP_LOGE("Matter", "esp_matter::start failed (0x%x)", err);
    return;
  }

  mStarted = true;
  ESP_LOGI("Matter", "Stack started");
  ESP_LOGI("Matter", "Passcode: %lu  Discriminator: %u",
           (unsigned long)mPasscode, mDiscriminator);

  char manual[13];
  buildManualCode(manual, sizeof(manual));
  ESP_LOGI("Matter", "Pairing code: %s", manual);
}

void matterOnWifiConnected() {
  if (mSuppressed) return;
  matterStart();
}

void matterLoop() {
  if (!mStarted) return;

  if (mFactoryResetPending) {
    mFactoryResetPending = false;
    doMatterFactoryReset();  // does not return — calls esp_restart()
  }

  applyPending();
  syncToMatter();
}

bool matterIsStarted() {
  return mStarted;
}

bool matterIsCommissioned() {
  // TODO: query Matter fabric state once commissioned
  // For now, return true if started (commissioning state would need
  // checking the fabric table)
  return mStarted;
}

const LightState& matterGetLightState(uint8_t index) {
  static LightState defaultState = {true, 254, 255, 255, 255, 0};
  if (index >= MAX_LIGHTS) return defaultState;
  return lightStates[index];
}

void matterReportState(uint8_t index, const LightState& state) {
  if (index >= MAX_LIGHTS) return;
  lightStates[index] = state;
  // syncToMatter() in the next loop iteration will push to Matter
}

void matterReconfigure() {
  reconfigPending = true;
  ESP_LOGW("Matter", "Light config changed. Restart required for Matter to pick up changes.");
}

uint16_t matterGetEndpointId(uint8_t index) {
  if (index >= MAX_LIGHTS) return 0;
  return endpoints[index].endpointId;
}

void matterGetPairingCode(char* out, size_t outLen) {
  buildManualCode(out, outLen);
}

void matterGetQRPayload(char* out, size_t outLen) {
  buildQRPayload(out, outLen);
}

void matterFactoryReset() {
  mFactoryResetPending = true;
}
