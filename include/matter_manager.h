/*
 * Matter WiFi WLED Bridge - Matter Manager
 *
 * Manages the Matter (Project CHIP) stack, exposing each configured WLED
 * device as a Matter Extended Color Light endpoint.
 *
 * Uses WiFi-only on-network commissioning (no BLE required).
 * Compatible with Apple Home, Google Home, and Amazon Alexa.
 *
 * Based on the Matter over WiFi usermod from:
 * https://github.com/netmindz/WLED-MM/tree/matter-over-wifi/usermods/matter_over_wifi/
 */

#pragma once

#include <Arduino.h>
#include "config_store.h"

// Initialize Matter node and endpoints (call from setup())
void matterSetup();

// Start the Matter stack (call after WiFi STA connects)
void matterStart();

// Process Matter state changes in the main loop
void matterLoop();

// Called when WiFi connects (starts Matter if deferred)
void matterOnWifiConnected();

// Check if Matter stack has started
bool matterIsStarted();

// Check if the device has been commissioned to a Matter fabric
bool matterIsCommissioned();

// Get the current state of a light (set by Matter commands)
const LightState& matterGetLightState(uint8_t index);

// Report a state change back to the Matter fabric (e.g. from web UI)
void matterReportState(uint8_t index, const LightState& state);

// Signal that light config has changed (requires restart)
void matterReconfigure();

// Get the 11-digit manual pairing code for a light endpoint
void matterGetPairingCode(char* out, size_t outLen);

// Get the QR code payload string (MT:XXXXX format)
void matterGetQRPayload(char* out, size_t outLen);

// Perform a Matter-only factory reset (preserves WiFi credentials)
void matterFactoryReset();
