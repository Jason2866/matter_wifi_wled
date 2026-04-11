/*
 * Matter WiFi WLED Bridge - WLED Device Discovery
 *
 * Discovers WLED devices on the local network using mDNS.
 * WLED devices advertise as _http._tcp with hostname typically "wled-XXXXXX".
 * After mDNS discovery, queries each device's /json/info endpoint to get
 * device name, LED count, RGBW capability, MAC address, etc.
 */

#pragma once

#include <Arduino.h>
#include <vector>

struct WledDeviceInfo {
  char name[64];       // WLED device name (from /json/info)
  char host[64];       // IP address as string
  uint16_t port;       // HTTP port (usually 80)
  char mac[18];        // MAC address (from /json/info)
  uint16_t ledCount;   // Number of LEDs
  bool isRGBW;         // Has white channel
  char version[16];    // WLED firmware version
};

/**
 * Initialize the mDNS subsystem for WLED discovery.
 * Must be called once after WiFi is connected (before or after Matter starts).
 * Safe to call multiple times — subsequent calls are no-ops.
 */
void wledDiscoveryInit();

/**
 * Scan the local network for WLED devices via mDNS.
 * Queries each discovered device's /json/info for metadata.
 * wledDiscoveryInit() must have been called first.
 *
 * @param results  Vector to fill with discovered devices
 * @param timeoutMs  mDNS scan timeout in milliseconds (default 5000)
 * @return Number of devices found
 */
int wledDiscover(std::vector<WledDeviceInfo>& results, uint32_t timeoutMs = 5000);
