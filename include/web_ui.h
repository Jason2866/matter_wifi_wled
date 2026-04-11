/*
 * Matter WiFi WLED Bridge - Web UI & WiFi Manager
 *
 * Handles:
 * - Initial WiFi configuration via captive portal (AP mode)
 * - Web-based configuration UI for WLED light definitions
 * - REST API for light configuration CRUD
 * - WLED device discovery via mDNS
 * - Status page showing Matter commissioning state
 */

#pragma once

#include <Arduino.h>

void webSetup();
void webLoop();

// Call when WiFi STA connects successfully
void webOnWifiConnected();

// WiFi connectivity check that works after Matter takes over esp_netif.
// Arduino's WiFi.status()/WiFi.isConnected() report disconnected (state 254)
// once Matter owns the WiFi interface.  This queries esp_netif directly.
bool isWiFiConnected();
