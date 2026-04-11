# Matter WiFi WLED Bridge

ESP32 firmware that bridges [Matter](https://csa-iot.org/all-solutions/matter/) smart home protocol to [WLED](https://kno.wled.ge/) LED controllers over WiFi. Each WLED device appears as a Matter Extended Color Light endpoint, controllable from Apple Home, Google Home, and Amazon Alexa.

```
Apple Home / Google Home / Alexa
        |
    (Matter over WiFi)
        |
     ESP32 bridge      ──(HTTP JSON API)──>  WLED device 1
        |                                  WLED device 2
    Web config UI                          WLED device N
```

## Features

- **Matter-compatible** — works with Apple Home, Google Home, and Alexa
- **Up to 16 WLED lights** — each exposed as an independent Matter endpoint
- **Full color control** — hue/saturation, CIE XY, and color temperature (mireds)
- **mDNS auto-discovery** — finds WLED devices on your network automatically
- **Web configuration UI** — captive portal for WiFi setup, device management
- **WiFi-only commissioning** — no Bluetooth/BLE required
- **OTA updates** — update firmware over the network via ArduinoOTA
- **Factory reset** — Matter-only reset preserves WiFi credentials

## Hardware

| Target | Board | Flash | Status |
|--------|-------|-------|--------|
| `esp32s3` (default) | ESP32-S3-DevKitC-1 | 8MB | Tested, recommended |
| `esp32s3_16mb` | ESP32-S3-DevKitC-1-N16R8 | 16MB | Tested |
| `esp32` | ESP32 DevKit | 8MB | Experimental |

All targets require **8MB flash** minimum (the firmware is ~2MB, and 8MB allows OTA with two 3MB app partitions). Classic ESP32 builds and links successfully but is considered experimental — IRAM is at ~82% utilisation. ESP32-C3/C6 may work but have not been tested.

## Quick Start

### Browser Install (Easiest)

Visit the [web installer](https://netmindz.github.io/matter_wifi_wled/) in Chrome or Edge to flash directly from your browser via USB.

### Build from Source

**Prerequisites:** [PlatformIO](https://platformio.org/) (CLI or IDE)

```bash
git clone https://github.com/netmindz/matter_wifi_wled.git
cd matter_wifi_wled
pio run -e esp32s3        # ESP32-S3 with 8MB flash (default)
# or: pio run -e esp32    # Classic ESP32 with 8MB flash
pio run -e esp32s3 -t upload
```

The first build takes several minutes as it downloads and compiles the ESP-IDF Matter SDK from source.

### Setup

1. Connect to the **MatterWLED-Setup** WiFi network
2. Configure your home WiFi in the captive portal
3. Open the device's IP in a browser to discover and add WLED devices
4. Pair with your smart home platform using the pairing code shown in the web UI

### Matter Pairing

The device uses WiFi-only on-network commissioning. After connecting to your WiFi network, it advertises via mDNS. Pair using:

- **Google Home** — Add device > Matter-enabled device > enter pairing code
- **Apple Home** — Add Accessory > scan QR code or enter manual code
- **Alexa** — Devices > Add Device > Other > Matter > enter pairing code

Default pairing code: `34970112332` (test credentials — change before production)

## Architecture

The firmware uses PlatformIO's **dual framework mode** (`arduino` + `espidf`). Arduino provides the web server, OTA, and WiFi management. ESP-IDF compiles the Matter SDK (`esp_matter`) from source via the IDF Component Manager, generating a proper sdkconfig with BLE disabled.

### Source Files

| File | Purpose |
|------|---------|
| `src/main.cpp` | Entry point, setup/loop, ArduinoOTA |
| `src/matter_manager.cpp` | Matter stack: node, endpoints, attribute callbacks |
| `src/web_ui.cpp` | Web server, REST API, captive portal, WiFi management |
| `src/config_store.cpp` | NVS persistence for light configuration |
| `src/wled_discovery.cpp` | mDNS discovery of WLED devices on the network |
| `src/wled_output.cpp` | HTTP POST to WLED `/json/state` API |

### Build Scripts

| Script | Purpose |
|--------|---------|
| `setup_matter_component.py` | Pre-script: copies `idf_component.yml`, patches TypeTraits.h |
| `generate_embed_files.py` | Post-script: generates `.S` cert embed files, patches TypeTraits.h |
| `fix_nodelist.py` | Pre-script: fixes SCons `NodeList` flattening in dual-framework builds |

### Key Build Decisions

- **No BLE** — `CONFIG_BT_ENABLED` is not set. WiFi-only commissioning avoids the BT controller crash that occurs with precompiled Arduino framework libs.
- **Shared mDNS stack** — `CONFIG_USE_MINIMAL_MDNS` is disabled so CHIP uses the ESP-IDF mDNS service instead of its own "minimal mDNS". This allows both Matter commissioning advertising and WLED device discovery (`mdns_query_ptr`) to share a single mDNS stack.
- **C++20 downgrade** — The Matter SDK's `TypeTraits.h` has broken C++23 syntax. CMakeLists.txt downgrades from `gnu++2b` to `gnu++20`.
- **Native HTTP client** — Uses ESP-IDF's `esp_http_client` instead of Arduino's `HTTPClient` to avoid `ssl_client` linker errors in dual-framework mode.
- **esp_netif for WiFi state** — After Matter starts, Arduino's `WiFi.status()` reports disconnected (state 254) even when connected. All WiFi state queries use `esp_netif` directly.

## Matter Factory Reset

Reset Matter commissioning state without losing WiFi credentials:

**Via web UI:** Use the factory reset button in the web interface.

**Via REST API:**
```bash
curl -X POST http://<device-ip>/api/matter/reset
```

This erases Matter fabric data (`chip-config`, `chip-counters`, `CHIP_KVS`, `esp_matter_kvs`, `node` NVS namespaces) and restarts the device. WiFi credentials are preserved.

## Partition Table (8MB Flash — Default)

| Partition | Offset | Size |
|-----------|--------|------|
| nvs | 0x9000 | 20KB |
| otadata | 0xE000 | 8KB |
| app0 (OTA 0) | 0x10000 | 3MB |
| app1 (OTA 1) | 0x310000 | 3MB |
| spiffs | 0x610000 | 256KB |
| coredump | 0x650000 | 64KB |

A 16MB variant is available in `partitions/matter_wled_16MB.csv` (used by the `esp32s3_16mb` environment) with the same app partitions plus a larger spiffs.

## Test Credentials

These are Matter test VID/PID values. They work for development and pairing with Google Home, Apple Home, and Alexa. Replace with allocated values before production.

| Parameter | Value |
|-----------|-------|
| Vendor ID | `0xFFF1` |
| Product ID | `0x8000` |
| Passcode | `20202021` |
| Discriminator | `3840` |

## Based On

This project is based on the [Matter over WiFi usermod](https://github.com/netmindz/WLED-MM/tree/matter-over-wifi/usermods/matter_over_wifi/) for WLED-MM, extracted into a standalone bridge firmware. See [research.md](research.md) for detailed technical notes on the implementation.

## License

MIT
