# Matter WiFi WLED Bridge — Development Research Notes

Hard-won discoveries from building a standalone Matter-to-WLED bridge on ESP32-S3 and ESP32.
Intended as a reference for future development, debugging, and porting work.

---

## Table of Contents

1. [Project Overview](#project-overview)
2. [Build System Architecture](#build-system-architecture)
3. [WiFi Conflict Between Arduino and Matter](#wifi-conflict-between-arduino-and-matter)
4. [Matter Color Control](#matter-color-control)
5. [Color Sync and Feedback Loop](#color-sync-and-feedback-loop)
6. [Device Naming in Matter](#device-naming-in-matter)
7. [NVS Layout and Factory Reset](#nvs-layout-and-factory-reset)
8. [Commissioning Credentials](#commissioning-credentials)
9. [Known Constraints and Limits](#known-constraints-and-limits)
10. [Crash History and Fixes](#crash-history-and-fixes)
11. [Key File Locations](#key-file-locations)

---

## Project Overview

This firmware exposes WLED LED controllers as **Matter Extended Color Light** endpoints
(device type `0x010D`) over WiFi only (no BLE/Bluetooth required). It is a standalone
ESP32 bridge — not a WLED usermod.

Built with:
- PlatformIO environments: `esp32s3` (default, 8MB), `esp32s3_16mb` (16MB), `esp32` (8MB, experimental)
- Framework: `arduino, espidf` (dual framework — both run simultaneously)
- Platform: `pioarduino/platform-espressif32 @ 55.03.37` (ESP-IDF 5.5.2)
- ESP-IDF component: `espressif/esp_matter` (pulled via `idf_component.yml`)
- Hardware: ESP32-S3-DevKitC-1 (8MB flash, default), ESP32-S3-DevKitC-1-N16R8 (16MB), ESP32 DevKit (8MB, experimental)

Matter clusters exposed per endpoint:
- `0x0006` On/Off
- `0x0008` Level Control → WLED brightness
- `0x0300` Color Control → WLED primary color (HSV + Color Temperature)

Communication with WLED devices uses ESP-IDF's `esp_http_client` to POST JSON to
each device's `/json/state` endpoint.

---

## Build System Architecture

### Dual framework (`arduino, espidf`)

When `framework = arduino, espidf` is specified in `platformio.ini`, PlatformIO runs
**two separate build systems simultaneously**:

1. **CMake / ESP-IDF** — compiles ESP-IDF components (including `esp_matter` and the
   entire CHIP/ConnectedHomeIP SDK). CMake does *not* compile Arduino or application code.
2. **SCons (PlatformIO)** — compiles Arduino framework code and application source files
   in `src/`. This is where `build_flags` apply.

This means:
- `CMakeLists.txt` controls flags for IDF components (e.g. `gnu++20` for the Matter SDK).
- `platformio.ini` `build_flags` controls flags for application code.
- You cannot use CMake `target_compile_options` to influence Arduino code, and vice versa.

### Why dual framework is required

With `framework = arduino` alone, PlatformIO uses **precompiled** ESP-IDF libraries that
have `CONFIG_BT_ENABLED=1` and `CONFIG_ENABLE_CHIPOBLE=1` baked in. Even though we don't
want BLE, the Matter SDK's BLE transport code compiles and the Bluetooth controller tries
to initialise at runtime — crashing with `Guru Meditation Error: LoadProhibited` in
`vQueueDelete` because the BT controller's memory region isn't mapped.

Switching to `framework = arduino, espidf` compiles esp_matter from source via the IDF
Component Manager, generating a proper sdkconfig with `CONFIG_BT_ENABLED` **not set**.

### CMake `main` component issue

PlatformIO's dual-framework mode names the `src/` directory component `"src"` not `"main"`,
but esp_matter's `CMakeLists.txt` calls `idf_component_get_property` for
`EXECUTABLE_COMPONENT_NAME` defaulting to `"main"`. Fix in root `CMakeLists.txt`:

```cmake
set(EXECUTABLE_COMPONENT_NAME "src")
```

This must appear before the `project()` call.

### C++23 / TypeTraits.h bug

esp_matter 1.4.0's `connectedhomeip/src/lib/support/TypeTraits.h` line 37 has broken
C++23 syntax. ESP-IDF 5.5.2 compiles with `-std=gnu++2b` (C++23) which triggers this.

Fixed with two complementary approaches:
1. **CMake-level downgrade** in `CMakeLists.txt`:
   ```cmake
   idf_build_replace_option_from_property(CXX_COMPILE_OPTIONS "-std=gnu++2b" "-std=gnu++20")
   ```
2. **Automated source patching** in `setup_matter_component.py` (pre-script) and
   `generate_embed_files.py` (post-script) — patches the header file after the IDF
   Component Manager downloads it.

### SCons `NodeList` crash (`fix_nodelist.py`)

In dual-framework builds, SCons sometimes returns `NodeList` objects instead of single
`Node` objects from `CollectBuildFiles`. This causes crashes during clean builds. The
`fix_nodelist.py` pre-script monkey-patches `CollectBuildFiles` to flatten `NodeList`
into individual nodes.

### `env["BUILD_DIR"]` returns unexpanded string

In PlatformIO `extra_scripts`, `env["BUILD_DIR"]` returns the raw SCons variable reference
(e.g. `"$BUILD_DIR"`) not the resolved path. Always use `env.subst("$BUILD_DIR")`.

### Embedded certificate files

The Matter SDK expects several `.crt` files to be available as embedded binary data
(assembly `.S` files). These don't exist at SCons configure time. The
`generate_embed_files.py` post-script generates the `.S` assembly wrapper files for:
- `https_server.crt`
- `mqtt_server.crt`
- `rmaker_mqtt_server.crt`
- `rmaker_claim_service_server.crt`
- `rmaker_ota_server.crt`

### `lib_ignore = Matter`

Arduino ESP32 3.x includes a built-in `Matter` library that conflicts with
`espressif/esp_matter` from the IDF Component Manager. Must be ignored in `platformio.ini`.

---

## WiFi Conflict Between Arduino and Matter

### The problem (WiFi state 254)

This was the **root cause of commissioning failures** and took the longest to diagnose.

Once `esp_matter::start()` is called, the Matter SDK takes ownership of the WiFi
`esp_netif` interface. After this point, Arduino's `WiFi.status()` reports
`WIFI_MODE_NULL` (state 254) and `WiFi.isConnected()` returns false — even though the
device is fully connected and functional at the IP layer.

Our web UI code (`webLoop()`) was using `WiFi.status() == WL_CONNECTED` to detect
connectivity changes. After Matter started, it reported "disconnected", causing the web
server to malfunction and preventing commissioning.

### The fix: `esp_netif` helpers

Created helper functions in `web_ui.cpp` that query `esp_netif` directly instead of
using Arduino WiFi:

```cpp
bool isWiFiConnected() {
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return false;
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(sta, &ip_info) != ESP_OK) return false;
    return ip_info.ip.addr != 0;
}
```

Also created `getStaIPString()` and `getStaSSID()` helpers. Replaced all
`WiFi.status()`, `WiFi.isConnected()`, `WiFi.localIP()`, `WiFi.SSID()` calls across
`web_ui.cpp`, `wled_output.cpp`, and `wled_discovery.cpp`.

After this fix, **Google Home pairing succeeded and control works**.

### ArduinoOTA mDNS conflict

`ArduinoOTA.begin()` calls `MDNS.begin()` which conflicts with the ESP-IDF mDNS service
(both bind UDP port 5353). Fixed by calling `ArduinoOTA.setMdnsEnabled(false)` before
`ArduinoOTA.begin()`.

### Shared mDNS stack (`CONFIG_USE_MINIMAL_MDNS=n`)

CHIP/Matter ships with two mDNS implementations for ESP32:

1. **"Minimal mDNS"** — CHIP's own C++ mDNS stack in
   `connectedhomeip/src/lib/dnssd/minimal_mdns/`. Binds port 5353 directly via lwIP
   `UDPEndPoint::Bind()`. Only handles Matter service types (`_matter._tcp`,
   `_matterc._udp`). This is the default when `CONFIG_USE_MINIMAL_MDNS=y`.

2. **ESP-IDF mDNS** — the `espressif__mdns` component. Uses `mdns_init()`, `mdns_query_ptr()`,
   `mdns_service_add()`, etc. CHIP wraps this in `ESP32DnssdImpl.cpp` → `EspDnssdInit()`.
   Used when `CONFIG_USE_MINIMAL_MDNS` is **not** set.

When `CONFIG_USE_MINIMAL_MDNS=y` (the default), WLED discovery via `mdns_query_ptr()`
fails because:
- CHIP's minimal mDNS binds port 5353 and handles Matter traffic only
- Our `wledDiscoveryInit()` creates a *separate* ESP-IDF mDNS instance that also binds 5353
- Even with `CONFIG_LWIP_SO_REUSE=y` and `CONFIG_LWIP_SO_REUSE_RXTOALL=y`, the ESP-IDF
  mDNS PCB never properly initialises — `mdns_query_ptr()` silently returns 0 results
  because `mdsn_priv_pcb_is_inited()` returns false (the PCB state gate in
  `mdns_querier.c:384` discards queries without error or logging)

**Fix**: Set `CONFIG_USE_MINIMAL_MDNS` to `n` in `sdkconfig.defaults`. Now CHIP uses
`EspDnssdInit()` which calls the ESP-IDF `mdns_init()`, creating a single shared mDNS
stack. Both Matter advertising (`_matter._tcp`, `_matterc._udp`) and WLED discovery
(`_http._tcp` PTR queries) go through the same ESP-IDF mDNS service.

Additional hardening in `wledDiscoveryInit()`:
- Calls `mdns_netif_action(sta, MDNS_EVENT_ENABLE_IP4)` to force the STA PCB to be
  enabled for IPv4, handling edge cases where WiFi reconnect left it in PCB_OFF state
- `wledDiscover()` has retry logic (up to 3 attempts with 2s delays) in case the PCB
  is still in the probing phase when the first query fires

The shared mDNS approach also saves ~24KB of flash by not linking the minimal mDNS code.

### WiFi AP mode fighting

`webSetup()` was starting in `WIFI_AP_STA` mode, causing WiFi mode toggling that confused
Matter's WiFi driver. Fixed by using `WIFI_STA` only when saved WiFi credentials exist.
AP mode is only used for initial WiFi configuration.

---

## Matter Color Control

### `hue_saturation` feature must be added explicitly

`esp_matter::endpoint::extended_color_light::create()` hardcodes only `color_temperature`
and `xy` features in the color_control cluster. The `hue_saturation` feature (capability
bit `0x01`) is **never added** by default.

Without it, Matter controllers (including Google Home) do not expose RGB color controls —
they only show a color temperature slider.

Fix: explicitly add the feature after endpoint creation but before `esp_matter::start()`:

```cpp
esp_matter::cluster_t *cc = esp_matter::cluster::get(endpoint, 0x0300);
if (cc) {
    esp_matter::cluster::color_control::feature::hue_saturation::config_t hsCfg;
    hsCfg.current_hue        = 0;
    hsCfg.current_saturation = 0;
    esp_matter::cluster::color_control::feature::hue_saturation::add(cc, &hsCfg);
}
```

### Color value ranges

| Domain | Hue | Saturation | Notes |
|--------|-----|------------|-------|
| Matter | 0–254 | 0–254 | 255 is reserved in both |
| WLED JSON API | — | — | Accepts RGB bytes directly |

Conversion (Matter → RGB) is done via standard HSV-to-RGB conversion with
`hue * 360.0 / 254.0` for the hue mapping.

Color temperature is converted from **mireds** (Matter `ColorTemperatureMired` attribute)
to RGB using the Tanner Helland algorithm (range 153–500 mireds = 6536K–2000K).

### RGBW decomposition

For WLED devices configured as RGBW, white channel extraction is performed:
```cpp
w = min(r, min(g, b));
r -= w; g -= w; b -= w;
```

---

## Color Sync and Feedback Loop

### The problem

`esp_matter::attribute::update()` fires `_attrCb` with `POST_UPDATE` **synchronously** in
the same call stack. Without protection, `syncToMatter()` calling `attribute::update()`
triggers `_attrCb`, which sets dirty flags, causing `applyPending()` to overwrite state on
the next loop iteration — a feedback loop.

### The fix: `mSyncing` flag

```cpp
static bool mSyncing = false;

static esp_err_t _attrCb(...) {
    if (mSyncing) return ESP_OK;  // suppress re-entry
    // ... set dirty flags ...
}

static void syncToMatter() {
    mSyncing = true;
    esp_matter::attribute::update(...);
    mSyncing = false;
}
```

### Per-attribute dirty flags

Separate dirty flags (`onDirty`, `briDirty`, `hsDirty`, `ctDirty`) ensure that
`applyPending()` only applies state that actually arrived from Matter. Without this,
a brightness command from Matter would also reset the color (because the stale pending
color value is zero).

### Thread safety

Matter attribute callbacks run on the **Matter RTOS task** (Core 0 on ESP32-S3). The
Arduino `loop()` runs on Core 1. The dirty-flag transfer model works without a mutex
because each flag is a single volatile boolean — the worst case is a one-frame delay.

If porting to a single-core chip, add `portENTER_CRITICAL` / `portEXIT_CRITICAL` around
the snapshot-and-clear step in `applyPending()`.

### Level Control minimum value

The Matter Level Control cluster has a minimum `current_level` value of 1. Attempting to
set it to 0 causes error `0x87` (`CONSTRAINT_ERROR`). Fix: only update `current_level`
when the light is on and brightness > 0. When off, the `on_off` attribute already
reflects the state and `current_level` retains its last-known brightness per Matter spec.

---

## Device Naming in Matter

### Problem: devices appear as "TEST_PRODUCT" or "Matter Device"

After pairing, devices appeared with generic names in Google Home instead of our custom names.

### Root cause

The `product_name` attribute on the root node's Basic Information cluster is
`MANAGED_INTERNALLY` — it's read from `DeviceInstanceInfoProvider`, not from the attribute
database. The default `ESP32FactoryDataProvider` reads from NVS namespace `"chip-factory"`,
keys `"vendor-name"` and `"product-name"`. When these don't exist, it returns "TEST_PRODUCT".

### Architecture of Matter naming

| Attribute | Location | Writable | Storage |
|-----------|----------|----------|---------|
| `node_label` | Basic Information cluster on root endpoint | Yes (controller can rename) | esp_matter attribute DB (NVS, nonvolatile) |
| `product_name` | Basic Information cluster on root endpoint | No (MANAGED_INTERNALLY) | `chip-factory` NVS namespace, key `"product-name"` |
| `vendor_name` | Basic Information cluster on root endpoint | No (MANAGED_INTERNALLY) | `chip-factory` NVS namespace, key `"vendor-name"` |

For **bridged** endpoints (if switching to bridge architecture later): `product_name`,
`vendor_name`, `node_label` on `bridged_device_basic_information` cluster are NOT managed
internally and can be set via the attribute API.

### The fix

Two changes in `matterSetup()`:

1. Set `node_label` via the config struct:
   ```cpp
   esp_matter::node::config_t nodeCfg;
   strncpy(nodeCfg.root_node.basic_information.node_label, "Matter WLED Bridge", 32);
   ```

2. Write vendor/product name to NVS before node creation:
   ```cpp
   nvs_handle_t h;
   if (nvs_open("chip-factory", NVS_READWRITE, &h) == ESP_OK) {
       nvs_set_str(h, "vendor-name", "Matter WLED");
       nvs_set_str(h, "product-name", "WLED Bridge");
       nvs_commit(h);
       nvs_close(h);
   }
   ```

**Note:** Requires Matter reset and re-pair for controllers to pick up new names.

---

## NVS Layout and Factory Reset

### NVS partition

All Matter and application data lives in the default `nvs` partition:
- Address: `0x9000`, size: `0x5000` (20KB)
- Partition label: `nvs`

### NVS namespaces

| Namespace | Contents | Source |
|-----------|----------|--------|
| `chip-factory` | Serial, certs, passcode, discriminator, Spake2p, vendor/product name | `ESP32Config.cpp` |
| `chip-config` | Commissioning state, fabric info | `ESP32Config.cpp` |
| `chip-counters` | Boot counters, reboot reason | `ESP32Config.cpp` |
| `CHIP_KVS` | Fabric keys, ACLs, group keys | `KeyValueStoreManagerImpl.h` |
| `esp_matter_kvs` | Attribute persistent storage | `esp_matter_nvs.h` |
| `node` | Minimum endpoint ID counter | `esp_matter_core.cpp` |
| `matter_wled` | Light configuration (application data) | `config_store.cpp` |

### Full NVS erase (nuclear option)

Erases **everything** including WiFi credentials and light config:
```bash
esptool.py --port /dev/ttyACM1 erase_region 0x9000 0x5000
```

### Selective Matter-only factory reset

Erases Matter commissioning state while preserving WiFi credentials and light config.
Available via the web UI or REST API.

Namespaces erased:

| Namespace | Contents erased |
|-----------|----------------|
| `chip-config` | Commissioning state, fabric info |
| `chip-counters` | Boot/reboot counters |
| `CHIP_KVS` | Fabric keys, ACLs, group keys |
| `esp_matter_kvs` | Persisted cluster attribute values |
| `node` | Endpoint ID counter |

`chip-factory` is deliberately **not** erased — it holds device attestation certificates
and the passcode/discriminator, which should survive recommissioning.

The CHIP SDK's own `ConfigurationManagerImpl::DoFactoryReset()` does the same thing but
also calls `esp_wifi_restore()` (which wipes WiFi credentials). We bypass it and use raw
`nvs.h` calls to preserve WiFi.

---

## Commissioning Credentials

### Test defaults (publicly known — change before production)

| Parameter | Default | Notes |
|-----------|---------|-------|
| Passcode | `20202021` | 8-digit Matter setup PIN |
| Discriminator | `3840` | 12-bit device discovery value |
| Vendor ID | `0xFFF1` | Matter test vendor ID |
| Product ID | `0x8000` | Matter test product ID |

### Manual pairing code (11-digit)

Computed per Matter Core Spec §5.1.4:
```
chunk1 (1 digit)  = discriminator >> 10
chunk2 (5 digits) = ((discriminator & 0x300) << 6) | (passcode & 0x3FFF)
chunk3 (4 digits) = (passcode >> 14) & 0x1FFF
check  (1 digit)  = Verhoeff checksum of digits 0..9
```

Default: `34970112332`

### QR payload (base-38)

Computed per Matter Core Spec §5.1.3. The 88-bit payload encodes vendor ID, product ID,
commissioning flow (0=Standard), rendezvous info (4=OnNetwork), discriminator, and
passcode into a base-38 string prefixed with `MT:`.

Default: `MT:Y.K90AFN00-W362MV6`

### mDNS advertisement

The device advertises on `_matterc._udp` when uncommissioned:
```
TXT: PI= PH=33 CM=1 D=3840 VP=65521+32768
```

---

## Known Constraints and Limits

### IRAM

IRAM usage varies by target:

| Target | IRAM Used | IRAM Free | Notes |
|--------|-----------|-----------|-------|
| ESP32-S3 | ~100% | ~0 | Near limit — avoid new `IRAM_ATTR` code |
| ESP32 (classic) | ~82% | ~24KB | Healthy margin |

If adding new code marked `IRAM_ATTR`, the linker will error with `IRAM segment overflow`. Mitigation:
- Remove `IRAM_ATTR` from non-ISR code
- Set `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=y` in sdkconfig

### Flash usage

Current firmware is ~1.95–1.98MB of a 3MB app partition (~62–63%). There is room for growth.
All targets use 8MB flash minimum with OTA support (two 3MB app partitions).

### RAM usage

~31–32% of SRAM used on both targets. ESP32-S3 N16R8 variant has 8MB PSRAM available for
heap allocations; classic ESP32 relies on internal 320KB SRAM only (no PSRAM unless board has it).

### Multi-target support

The firmware compiles and links successfully for both ESP32-S3 and classic ESP32:

| Target | Flash (of 3MB) | IRAM | DRAM | Status |
|--------|---------------|------|------|--------|
| `esp32s3` | 63% (1.98MB) | ~100% | 32% | Production ready |
| `esp32s3_16mb` | 63% (1.98MB) | ~100% | 32% | Production ready |
| `esp32` | 62% (1.95MB) | 82% | 31% | Experimental |

Classic ESP32 notes:
- Bootloader offset is `0x1000` (vs `0x0` on ESP32-S3)
- Flash mode is `dio` (vs `qio` on ESP32-S3)
- Dual-core, so meets Matter's multi-thread requirements
- No runtime testing yet — builds and links but not verified on hardware

### ESP32 compatibility

ESP32-S3 and classic ESP32 are supported. ESP32-C3, C6, and H2 may work but have not been
tested with this firmware.

### Arduino `HTTPClient` not usable

Arduino's `HTTPClient` pulls in `NetworkClientSecure` which depends on `ssl_client.cpp`.
This doesn't link in dual-framework mode. Use ESP-IDF's `esp_http_client` instead.

### Flash layout differences between targets

The bootloader offset differs between ESP32 variants. This matters for the web installer
and CI artifacts:

| Target | Bootloader Offset | Flash Mode | Notes |
|--------|------------------|------------|-------|
| ESP32-S3 | `0x0` | qio | Uses merged binary for web install |
| ESP32 (classic) | `0x1000` | dio | Different merge offset |

Both use partition table at `0x8000`, OTA data at `0xE000`, and app at `0x10000`.

---

## Crash History and Fixes

These are the major issues encountered and resolved during development, in chronological
order. Documented here so future developers don't have to rediscover them.

### 1. BLE crash — `Guru Meditation Error: LoadProhibited` in `vQueueDelete`

**Cause:** `framework = arduino` uses precompiled ESP-IDF libs with `CONFIG_BT_ENABLED=1`.
The BT controller tries to initialise but its memory region isn't mapped on our sdkconfig.

**Fix:** Switch to `framework = arduino, espidf` to compile esp_matter from source with
BLE disabled.

### 2. CMake component resolution — `"main" component not found`

**Cause:** PlatformIO names the src component `"src"`, not `"main"`.

**Fix:** `set(EXECUTABLE_COMPONENT_NAME "src")` in `CMakeLists.txt`.

### 3. FreeRTOS HZ mismatch — Arduino timing broken

**Cause:** Arduino requires `CONFIG_FREERTOS_HZ=1000` but IDF defaults to 100.

**Fix:** Set in `sdkconfig.defaults`.

### 4. Missing `https_server.crt.S` — linker error

**Cause:** Matter SDK expects embedded cert files that don't exist at configure time.

**Fix:** `generate_embed_files.py` post-script generates the `.S` wrapper files.

### 5. TypeTraits.h — C++23 compilation error

**Cause:** Broken syntax in CHIP SDK header with `-std=gnu++2b`.

**Fix:** CMake-level downgrade to `gnu++20` + automated source patching.

### 6. `ssl_client` linker errors

**Cause:** Arduino `HTTPClient` → `NetworkClientSecure` → `ssl_client.cpp` doesn't link
in dual-framework mode.

**Fix:** Replaced with ESP-IDF's `esp_http_client`.

### 7. mDNS port conflict

**Cause:** `ArduinoOTA.begin()` calls `MDNS.begin()` which binds UDP 5353, conflicting
with the mDNS service used by CHIP.

**Fix:** `ArduinoOTA.setMdnsEnabled(false)`.

### 8. WiFi state 254 — commissioning failure (the big one)

**Cause:** After Matter starts, Arduino's `WiFi.status()` reports disconnected. Web server
and connectivity checks break.

**Fix:** Replace all Arduino WiFi state queries with `esp_netif` direct queries.

### 9. mDNS WLED discovery returns 0 results

**Cause:** `CONFIG_USE_MINIMAL_MDNS=y` (the default) makes CHIP use its own minimal mDNS
on port 5353. Our `mdns_query_ptr()` uses the separate ESP-IDF mDNS stack which never
properly initialises its PCB — queries are silently discarded.

**Fix:** Set `CONFIG_USE_MINIMAL_MDNS` to `n` so CHIP uses the ESP-IDF mDNS service.
Both Matter advertising and WLED discovery now share one mDNS stack. Added
`mdns_netif_action(ENABLE_IP4)` safety net and query retry logic.

### 10. Error 0x87 on Level Control

**Cause:** `syncToMatter()` set `current_level` to 0 when light is off. Level Control
minimum is 1.

**Fix:** Only update level when on and brightness > 0.

### 11. SCons `NodeList` crash

**Cause:** Dual-framework builds sometimes return `NodeList` instead of `Node` from
`CollectBuildFiles`.

**Fix:** `fix_nodelist.py` monkey-patches the function to flatten NodeLists.

---

## Key File Locations

### Build configuration

| File | Purpose |
|------|---------|
| `platformio.ini` | Board, framework, dependencies, build flags (3 envs: esp32s3, esp32s3_16mb, esp32) |
| `CMakeLists.txt` | IDF component build, C++20 downgrade |
| `sdkconfig.defaults` | FreeRTOS HZ, Arduino autostart, mbedtls HKDF, shared mDNS (`USE_MINIMAL_MDNS=n`) |
| `idf_component.yml` | ESP-IDF component manifest (esp_matter dependency) |
| `partitions/matter_wled_8MB.csv` | 8MB flash layout (default — used by esp32s3 and esp32) |
| `partitions/matter_wled_16MB.csv` | 16MB flash layout (used by esp32s3_16mb) |
| `matter_gcc14_compat.h` | GCC 14 compat header (placeholder) |

### Build scripts

| File | Purpose |
|------|---------|
| `setup_matter_component.py` | Copies idf_component.yml to src/, patches TypeTraits.h |
| `generate_embed_files.py` | Generates .S cert files, patches TypeTraits.h post-CMake |
| `fix_nodelist.py` | Monkey-patches CollectBuildFiles for NodeList flattening |

### Source files

| File | Purpose |
|------|---------|
| `src/main.cpp` | Entry point, setup/loop, ArduinoOTA |
| `src/matter_manager.cpp` | Matter stack: node, endpoints, callbacks, pairing codes |
| `src/web_ui.cpp` | Web server, REST API, WiFi manager, esp_netif helpers |
| `src/config_store.cpp` | NVS persistence for light configuration |
| `src/wled_discovery.cpp` | mDNS WLED device discovery |
| `src/wled_output.cpp` | HTTP POST to WLED JSON API |

### CI/CD and installer

| File | Purpose |
|------|---------|
| `.github/workflows/build.yml` | GitHub Actions: build, release, Pages deployment |
| `docs/install/index.html` | ESP Web Tools browser-based installer |

### Do not modify

| Path | Reason |
|------|--------|
| `managed_components/` | Auto-downloaded by IDF Component Manager; reverts on next build |
| `sdkconfig.esp32s3` | Generated by CMake; overwritten on every build |
| `sdkconfig.esp32` | Generated by CMake; overwritten on every build |
| `dependencies.lock` | Generated by IDF Component Manager |
