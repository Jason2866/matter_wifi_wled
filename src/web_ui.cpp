/*
 * Matter WiFi WLED Bridge - Web UI & WiFi Manager Implementation
 *
 * Provides:
 * - Captive portal AP for initial WiFi setup
 * - Configuration web UI for light definitions with WLED device discovery
 * - REST API: GET/POST /api/config, POST /api/factory-reset
 * - WLED discovery API: GET /api/wled/discover
 * - Status API: GET /api/status (includes Matter pairing info)
 *
 * Uses the built-in synchronous WebServer (not ESPAsyncWebServer).
 */

#include "web_ui.h"
#include "config_store.h"
#include "wled_output.h"
#include "wled_discovery.h"
#include "matter_manager.h"

#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_netif.h>

static WebServer server(80);
static DNSServer dnsServer;
static bool apMode = false;
static bool serverStarted = false;

// WiFi credentials stored in NVS
static String wifiSSID;
static String wifiPass;

// ── WiFi connectivity check ─────────────────────────────────────────────────
// Once Matter starts, it takes ownership of the WiFi esp_netif interface.
// Arduino's WiFi.status() may report WIFI_MODE_NULL (state 254) even though
// the device is fully connected at the IP layer.  Query esp_netif directly
// to get the true connection state (same approach as WLED-MM's Network.cpp
// with the externalWiFiManager flag).
bool isWiFiConnected() {
  // If Matter hasn't started yet, Arduino's WiFi object is still authoritative
  if (!matterIsStarted()) {
    return WiFi.status() == WL_CONNECTED;
  }
  // After Matter starts, query esp_netif directly
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (sta) {
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(sta, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
      return true;
    }
  }
  return false;
}

// Get the STA IP address string, querying esp_netif when Matter owns WiFi.
static String getStaIPString() {
  if (!matterIsStarted()) {
    return WiFi.localIP().toString();
  }
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (sta) {
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(sta, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
      char buf[16];
      esp_ip4addr_ntoa(&ip_info.ip, buf, sizeof(buf));
      return String(buf);
    }
  }
  return String("0.0.0.0");
}

// Get the SSID, falling back to stored credentials when Arduino can't report it.
static String getStaSSID() {
  if (!matterIsStarted()) {
    return WiFi.SSID();
  }
  // After Matter takes over WiFi, WiFi.SSID() may return empty.
  // Use the stored credential as fallback.
  String ssid = WiFi.SSID();
  if (ssid.length() == 0) {
    ssid = wifiSSID;
  }
  return ssid;
}

// ---- HTML served from PROGMEM ----
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Matter WLED Bridge</title>
<link rel="icon" type="image/svg+xml" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E%3Crect width='32' height='32' rx='6' fill='%231a1a2e'/%3E%3Cpath d='M18 4L8 18h6l-2 10 10-14h-6z' fill='%2300d4ff'/%3E%3C/svg%3E">
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
         background: #1a1a2e; color: #e0e0e0; max-width: 800px; margin: 0 auto; padding: 16px; }
  h1 { color: #00d4ff; margin-bottom: 8px; font-size: 1.4em; }
  h2 { color: #aaa; font-size: 1em; margin-bottom: 16px; font-weight: normal; }
  .card { background: #16213e; border-radius: 8px; padding: 16px; margin-bottom: 12px;
          border: 1px solid #0f3460; }
  .status { display: flex; gap: 16px; flex-wrap: wrap; margin-bottom: 16px; }
  .status-item { background: #0f3460; padding: 8px 16px; border-radius: 6px; }
  .status-item .label { font-size: 0.8em; color: #888; }
  .status-item .value { font-size: 1.1em; color: #00d4ff; }
  .light { display: grid; grid-template-columns: auto 1fr auto; gap: 8px; align-items: center; }
  .light-preview { width: 36px; height: 36px; border-radius: 6px; border: 2px solid #0f3460;
                   background: #000; flex-shrink: 0; }
  .light-info { display: flex; flex-direction: column; gap: 4px; }
  .light-name { font-weight: bold; color: #e0e0e0; }
  .light-detail { font-size: 0.85em; color: #888; }
  .btn { padding: 8px 16px; border: none; border-radius: 6px; cursor: pointer;
         font-size: 0.9em; color: #fff; }
  .btn-primary { background: #00d4ff; color: #1a1a2e; }
  .btn-danger { background: #e74c3c; }
  .btn-sm { padding: 4px 10px; font-size: 0.8em; }
  .btn:disabled { opacity: 0.5; cursor: not-allowed; }
  input, select { background: #1a1a2e; color: #e0e0e0; border: 1px solid #0f3460;
                  border-radius: 4px; padding: 6px 10px; font-size: 0.9em; width: 100%; }
  label { font-size: 0.85em; color: #aaa; display: block; margin-bottom: 4px; }
  .form-group { margin-bottom: 10px; }
  .form-row { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
  .modal { display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%;
           background: rgba(0,0,0,0.7); z-index: 100; justify-content: center; align-items: center; }
  .modal.active { display: flex; }
  .modal-content { background: #16213e; border-radius: 8px; padding: 24px;
                   max-width: 500px; width: 90%; border: 1px solid #0f3460;
                   max-height: 90vh; overflow-y: auto; }
  .modal-title { font-size: 1.1em; margin-bottom: 16px; color: #00d4ff; }
  .modal-actions { display: flex; gap: 8px; justify-content: flex-end; margin-top: 16px; }
  .wifi-form { margin-top: 16px; }
  #lightList { min-height: 40px; }
  .empty-state { text-align: center; padding: 24px; color: #666; }
  .actions { display: flex; gap: 8px; margin-top: 16px; }
  .tag { display: inline-block; padding: 2px 8px; border-radius: 4px;
         font-size: 0.75em; font-weight: bold; }
  .tag-rgb { background: #2d1f6e; color: #a78bfa; }
  .tag-rgbw { background: #1f4e6e; color: #7bc8fa; }
  .hidden { display: none; }
  .wled-device { background: #0f3460; border-radius: 6px; padding: 12px; margin: 8px 0;
                 cursor: pointer; border: 2px solid transparent; transition: border-color 0.2s; }
  .wled-device:hover { border-color: #00d4ff; }
  .wled-device.selected { border-color: #00d4ff; background: #1a3a6e; }
  .wled-device .dev-name { font-weight: bold; color: #e0e0e0; }
  .wled-device .dev-detail { font-size: 0.85em; color: #888; margin-top: 4px; }
  .discover-status { text-align: center; padding: 12px; color: #888; font-size: 0.9em; }
  .spinner { display: inline-block; width: 16px; height: 16px; border: 2px solid #888;
             border-top-color: #00d4ff; border-radius: 50%; animation: spin 0.8s linear infinite; }
  @keyframes spin { to { transform: rotate(360deg); } }
  .pairing-info { background: #0f3460; border-radius: 6px; padding: 12px; margin-top: 8px; font-family: monospace; }
  .pairing-info .code { font-size: 1.3em; color: #00d4ff; letter-spacing: 2px; }
  .pairing-info .qr-link { font-size: 0.85em; color: #aaa; margin-top: 4px; }
  .pairing-info .qr-link a { color: #00d4ff; }
</style>
</head>
<body>
<h1>Matter WLED Bridge</h1>
<h2>ESP32-S3 Matter-to-WLED Light Controller</h2>

<div class="status" id="statusBar">
  <div class="status-item">
    <div class="label">WiFi</div>
    <div class="value" id="wifiStatus">...</div>
  </div>
  <div class="status-item">
    <div class="label">Matter</div>
    <div class="value" id="matterStatus">...</div>
  </div>
  <div class="status-item">
    <div class="label">Lights</div>
    <div class="value" id="lightCountStatus">0</div>
  </div>
</div>

<!-- Matter pairing info -->
<div class="card" id="pairingCard" style="display:none">
  <h3 style="color:#00d4ff;margin-bottom:12px">Matter Pairing</h3>
  <p style="font-size:0.9em;color:#aaa;margin-bottom:8px">Use this code to add the device in Apple Home, Google Home, or Alexa:</p>
  <div class="pairing-info">
    <div>Manual code: <span class="code" id="pairingCode">...</span></div>
    <div class="qr-link" id="qrLink"></div>
  </div>
</div>

<!-- WiFi setup (shown in AP mode) -->
<div class="card" id="wifiSetup" style="display:none">
  <h3 style="color:#00d4ff;margin-bottom:12px">WiFi Setup</h3>
  <div class="wifi-form">
    <div class="form-group">
      <label>SSID</label>
      <input type="text" id="wifiSSID" placeholder="Your WiFi network name">
    </div>
    <div class="form-group">
      <label>Password</label>
      <input type="password" id="wifiPass" placeholder="WiFi password">
    </div>
    <button class="btn btn-primary" onclick="saveWifi()">Connect</button>
  </div>
</div>

<!-- Lights list -->
<div class="card">
  <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:12px">
    <h3 style="color:#00d4ff">Lights</h3>
    <button class="btn btn-primary btn-sm" onclick="showAddLight()">+ Add Light</button>
  </div>
  <div id="lightList">
    <div class="empty-state" id="loadingState">Loading configuration...</div>
  </div>
</div>

<!-- Actions -->
<div class="actions">
  <button class="btn btn-sm" style="background:#0f3460" onclick="restartDevice()">Restart Device</button>
  <button class="btn btn-sm" style="background:#8b5e00" onclick="matterReset()">Matter Reset</button>
  <button class="btn btn-danger btn-sm" onclick="factoryReset()">Factory Reset</button>
</div>

<!-- OTA Firmware Update -->
<div class="card" style="margin-top:12px">
  <h3 style="color:#00d4ff;margin-bottom:12px">Firmware Update</h3>
  <form id="otaForm" method="POST" action="/api/ota" enctype="multipart/form-data">
    <div class="form-group">
      <label>Select firmware binary (.bin)</label>
      <input type="file" name="firmware" accept=".bin" id="otaFile">
    </div>
    <button class="btn btn-primary btn-sm" type="button" onclick="uploadOTA()">Upload &amp; Flash</button>
    <div id="otaProgress" style="margin-top:8px;display:none">
      <div style="background:#0f3460;border-radius:4px;overflow:hidden;height:24px">
        <div id="otaBar" style="background:#00d4ff;height:100%;width:0%;transition:width 0.3s"></div>
      </div>
      <div id="otaStatus" style="font-size:0.85em;color:#aaa;margin-top:4px">Uploading...</div>
    </div>
  </form>
</div>

<!-- Add/Edit Light Modal -->
<div class="modal" id="lightModal">
  <div class="modal-content">
    <div class="modal-title" id="modalTitle">Add Light</div>
    <input type="hidden" id="editIndex" value="-1">
    <div class="form-group">
      <label>Name</label>
      <input type="text" id="lightName" placeholder="e.g. Living Room Strip">
    </div>
    <div class="form-row">
      <div class="form-group">
        <label>Type</label>
        <select id="lightType">
          <option value="RGB">RGB</option>
          <option value="RGBW">RGBW</option>
        </select>
      </div>
      <div class="form-group">
        <label>WLED Port</label>
        <input type="number" id="wledPort" min="1" max="65535" value="80">
      </div>
    </div>
    <div class="form-group">
      <label>WLED Device IP / Hostname</label>
      <input type="text" id="wledHost" placeholder="e.g. 192.168.1.50 or wled-abcdef.local">
    </div>

    <!-- WLED Discovery section -->
    <div style="margin-top:12px;padding-top:12px;border-top:1px solid #0f3460">
      <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:8px">
        <label style="margin:0">Or discover WLED devices on network:</label>
        <button class="btn btn-sm" style="background:#0f3460" onclick="discoverWled()" id="discoverBtn">Scan</button>
      </div>
      <div id="discoveryResults">
        <div class="discover-status" id="discoveryStatus">Click "Scan" to find WLED devices</div>
      </div>
    </div>

    <div class="modal-actions">
      <button class="btn" style="background:#444" onclick="closeModal()">Cancel</button>
      <button class="btn btn-primary" onclick="saveLight()">Save</button>
    </div>
  </div>
</div>

<script>
let config = { lights: [] };
let isApMode = false;

async function loadStatus() {
  try {
    const r = await fetch('/api/status');
    const s = await r.json();
    document.getElementById('wifiStatus').textContent = s.wifi || 'Unknown';
    document.getElementById('matterStatus').textContent = s.matter || 'Unknown';
    isApMode = s.apMode || false;
    document.getElementById('wifiSetup').style.display = isApMode ? 'block' : 'none';

    // Show pairing info when Matter is running
    const pairingCard = document.getElementById('pairingCard');
    if (s.pairingCode) {
      pairingCard.style.display = 'block';
      document.getElementById('pairingCode').textContent = s.pairingCode;
      if (s.qrPayload) {
        const qrUrl = 'https://api.qrserver.com/v1/create-qr-code/?size=300x300&data=' + encodeURIComponent(s.qrPayload);
        document.getElementById('qrLink').innerHTML =
          'QR: <a href="' + qrUrl + '" target="_blank">' + escHtml(s.qrPayload) + '</a>';
      }
    } else {
      pairingCard.style.display = 'none';
    }
  } catch(e) {
    document.getElementById('wifiStatus').textContent = 'AP Mode';
    document.getElementById('wifiSetup').style.display = 'block';
    isApMode = true;
  }
}

async function loadConfig() {
  try {
    const r = await fetch('/api/config');
    config = await r.json();
    renderLights();
  } catch(e) {
    console.error('Failed to load config:', e);
  }
}

async function loadLightStates() {
  try {
    const r = await fetch('/api/lights/state');
    const data = await r.json();
    const lights = data.lights || [];
    for (let i = 0; i < lights.length; i++) {
      const el = document.getElementById('preview-' + i);
      if (!el) continue;
      const l = lights[i];
      if (l.on) {
        const pw = l.w || 0;
        const pr = Math.min(255, l.r + pw);
        const pg = Math.min(255, l.g + pw);
        const pb = Math.min(255, l.b + pw);
        el.style.background = 'rgb(' + pr + ',' + pg + ',' + pb + ')';
        el.style.boxShadow = '0 0 8px rgba(' + pr + ',' + pg + ',' + pb + ',0.5)';
      } else {
        el.style.background = '#111';
        el.style.boxShadow = 'none';
      }
    }
  } catch(e) { /* ignore */ }
}

function renderLights() {
  const list = document.getElementById('lightList');
  document.getElementById('lightCountStatus').textContent = config.lights.length;

  if (config.lights.length === 0) {
    list.innerHTML = '<div class="empty-state" id="emptyState">No lights configured. Add one to get started.</div>';
    return;
  }

  list.innerHTML = config.lights.map((l, i) => {
    const tagClass = l.type === 'RGBW' ? 'tag-rgbw' : 'tag-rgb';
    return `<div class="card light">
      <div class="light-preview" id="preview-${i}"></div>
      <div class="light-info">
        <div class="light-name">${escHtml(l.name)} <span class="tag ${tagClass}">${l.type}</span></div>
        <div class="light-detail">WLED: ${escHtml(l.wledHost || '(not set)')}:${l.wledPort || 80} | Endpoint ${i + 1}</div>
      </div>
      <div>
        <button class="btn btn-sm" style="background:#0f3460" onclick="editLight(${i})">Edit</button>
        <button class="btn btn-danger btn-sm" onclick="deleteLight(${i})">Del</button>
      </div>
    </div>`;
  }).join('');
}

function escHtml(s) {
  const d = document.createElement('div');
  d.textContent = s;
  return d.innerHTML;
}

function showAddLight() {
  document.getElementById('modalTitle').textContent = 'Add Light';
  document.getElementById('editIndex').value = -1;
  document.getElementById('lightName').value = 'Light ' + (config.lights.length + 1);
  document.getElementById('lightType').value = 'RGB';
  document.getElementById('wledHost').value = '';
  document.getElementById('wledPort').value = 80;
  clearDiscovery();
  document.getElementById('lightModal').classList.add('active');
}

function editLight(i) {
  const l = config.lights[i];
  document.getElementById('modalTitle').textContent = 'Edit Light';
  document.getElementById('editIndex').value = i;
  document.getElementById('lightName').value = l.name;
  document.getElementById('lightType').value = l.type;
  document.getElementById('wledHost').value = l.wledHost || '';
  document.getElementById('wledPort').value = l.wledPort || 80;
  clearDiscovery();
  document.getElementById('lightModal').classList.add('active');
}

function closeModal() {
  document.getElementById('lightModal').classList.remove('active');
}

function clearDiscovery() {
  document.getElementById('discoveryResults').innerHTML =
    '<div class="discover-status" id="discoveryStatus">Click "Scan" to find WLED devices</div>';
}

async function discoverWled() {
  const btn = document.getElementById('discoverBtn');
  const results = document.getElementById('discoveryResults');
  btn.disabled = true;
  btn.textContent = 'Scanning...';
  results.innerHTML = '<div class="discover-status"><span class="spinner"></span> Scanning network for WLED devices...</div>';

  try {
    const r = await fetch('/api/wled/discover');
    const data = await r.json();
    const devices = data.devices || [];

    if (devices.length === 0) {
      results.innerHTML = '<div class="discover-status">No WLED devices found. Make sure they are on the same network.</div>';
    } else {
      results.innerHTML = devices.map((d, i) =>
        `<div class="wled-device" onclick="selectWledDevice(this, '${escAttr(d.host)}', ${d.port}, '${escAttr(d.name)}', ${d.isRGBW ? 'true' : 'false'})">
          <div class="dev-name">${escHtml(d.name)}</div>
          <div class="dev-detail">${escHtml(d.host)}:${d.port} | ${d.ledCount} LEDs | ${d.isRGBW ? 'RGBW' : 'RGB'} | v${escHtml(d.version)}</div>
        </div>`
      ).join('');
    }
  } catch(e) {
    results.innerHTML = '<div class="discover-status" style="color:#e74c3c">Discovery failed: ' + escHtml(e.message) + '</div>';
  }

  btn.disabled = false;
  btn.textContent = 'Scan';
}

function escAttr(s) {
  return s.replace(/'/g, "\\'").replace(/"/g, '&quot;');
}

function selectWledDevice(el, host, port, name, isRGBW) {
  document.querySelectorAll('.wled-device').forEach(d => d.classList.remove('selected'));
  el.classList.add('selected');
  document.getElementById('wledHost').value = host;
  document.getElementById('wledPort').value = port;
  document.getElementById('lightType').value = isRGBW ? 'RGBW' : 'RGB';
  const nameField = document.getElementById('lightName');
  if (nameField.value.match(/^Light \d+$/)) {
    nameField.value = name;
  }
}

async function saveLight() {
  const idx = parseInt(document.getElementById('editIndex').value);
  const light = {
    name: document.getElementById('lightName').value || 'Light',
    type: document.getElementById('lightType').value,
    wledHost: document.getElementById('wledHost').value.trim(),
    wledPort: parseInt(document.getElementById('wledPort').value) || 80,
  };

  if (!light.wledHost) {
    alert('Please enter a WLED device IP or hostname, or use discovery to find one.');
    return;
  }

  if (idx >= 0) {
    config.lights[idx] = light;
  } else {
    config.lights.push(light);
  }

  closeModal();
  await postConfig();
  renderLights();
}

async function deleteLight(i) {
  if (!confirm('Delete "' + config.lights[i].name + '"?')) return;
  config.lights.splice(i, 1);
  await postConfig();
  renderLights();
}

async function postConfig() {
  try {
    await fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(config)
    });
  } catch(e) {
    alert('Failed to save configuration');
  }
}

async function saveWifi() {
  const ssid = document.getElementById('wifiSSID').value;
  const pass = document.getElementById('wifiPass').value;
  if (!ssid) { alert('Please enter SSID'); return; }

  try {
    await fetch('/api/wifi', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ssid, password: pass })
    });
    alert('WiFi credentials saved. The device will now restart and connect to your network.');
  } catch(e) {
    alert('Failed to save WiFi settings');
  }
}

async function factoryReset() {
  if (!confirm('This will erase all configuration including WiFi, Matter pairing, and light settings. Continue?')) return;
  try {
    await fetch('/api/factory-reset', { method: 'POST' });
    alert('Factory reset complete. Device will restart.');
  } catch(e) {
    alert('Failed to factory reset');
  }
}

async function matterReset() {
  if (!confirm('This will unpair the device from all Matter controllers (Apple Home, Google Home, etc). WiFi and light settings are preserved. Continue?')) return;
  try {
    await fetch('/api/matter-reset', { method: 'POST' });
    alert('Matter reset complete. Device will restart and can be re-paired.');
  } catch(e) {
    alert('Failed to reset Matter');
  }
}

async function restartDevice() {
  if (!confirm('Restart the device?')) return;
  try {
    await fetch('/api/restart', { method: 'POST' });
    document.getElementById('wifiStatus').textContent = 'Restarting...';
    document.getElementById('matterStatus').textContent = '...';
  } catch(e) { /* expected - device is restarting */ }
}

function uploadOTA() {
  const fileInput = document.getElementById('otaFile');
  if (!fileInput.files.length) { alert('Please select a firmware file'); return; }
  if (!confirm('Upload and flash firmware? The device will restart after flashing.')) return;

  const file = fileInput.files[0];
  const formData = new FormData();
  formData.append('firmware', file);

  const xhr = new XMLHttpRequest();
  const progress = document.getElementById('otaProgress');
  const bar = document.getElementById('otaBar');
  const status = document.getElementById('otaStatus');

  progress.style.display = 'block';
  status.textContent = 'Uploading...';
  bar.style.width = '0%';

  xhr.upload.addEventListener('progress', function(e) {
    if (e.lengthComputable) {
      const pct = Math.round((e.loaded / e.total) * 100);
      bar.style.width = pct + '%';
      status.textContent = 'Uploading: ' + pct + '%';
    }
  });

  xhr.addEventListener('load', function() {
    if (xhr.status === 200) {
      bar.style.width = '100%';
      status.textContent = 'Flashing complete! Device is restarting...';
      status.style.color = '#00d4ff';
    } else {
      status.textContent = 'Upload failed: ' + xhr.responseText;
      status.style.color = '#e74c3c';
    }
  });

  xhr.addEventListener('error', function() {
    status.textContent = 'Upload failed (network error)';
    status.style.color = '#e74c3c';
  });

  xhr.open('POST', '/api/ota');
  xhr.send(formData);
}

// Poll helpers
function pollStatus() {
  loadStatus().finally(() => setTimeout(pollStatus, 5000));
}
function pollLightStates() {
  loadLightStates().finally(() => setTimeout(pollLightStates, 1000));
}

// Initial load
Promise.all([loadStatus(), loadConfig()]).then(() => {
  document.getElementById('loadingState')?.remove();
  setTimeout(pollStatus, 5000);
  setTimeout(pollLightStates, 1000);
});
</script>
</body>
</html>
)rawliteral";

// ---- Helper: load WiFi creds from NVS ----
static void loadWifiCreds() {
  Preferences p;
  if (!p.begin("mtwled_wifi", false)) {
    ESP_LOGW("Web", "WiFi NVS namespace init failed");
    return;
  }
  wifiSSID = p.getString("ssid", "");
  wifiPass = p.getString("pass", "");
  p.end();
}

static void saveWifiCreds(const String& ssid, const String& pass) {
  Preferences p;
  p.begin("mtwled_wifi", false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
}

// ---- Setup API routes ----
static void setupRoutes() {
  // Serve the main page
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", INDEX_HTML);
  });

  // Status API
  server.on("/api/status", HTTP_GET, []() {
    JsonDocument doc;
    doc["wifi"] = isWiFiConnected() ? getStaSSID() : "Not connected";
    if (isWiFiConnected()) {
      doc["ip"] = getStaIPString();
    }
    doc["apMode"] = apMode;

    if (configStore.getLightCount() == 0) {
      doc["matter"] = "Disabled - no lights configured";
    } else if (matterIsStarted()) {
      doc["matter"] = "Running";
    } else {
      doc["matter"] = "Waiting for WiFi...";
    }

    doc["lightCount"] = configStore.getLightCount();

    // Include pairing info if Matter is set up
    if (matterIsStarted()) {
      char code[13];
      matterGetPairingCode(code, sizeof(code));
      doc["pairingCode"] = code;

      char qr[28];
      matterGetQRPayload(qr, sizeof(qr));
      doc["qrPayload"] = qr;
    }

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  });

  // Light states API - returns current RGB output per light
  server.on("/api/lights/state", HTTP_GET, []() {
    JsonDocument doc;
    JsonArray arr = doc["lights"].to<JsonArray>();
    uint8_t count = configStore.getLightCount();
    for (uint8_t i = 0; i < count; i++) {
      const LightState& st = matterGetLightState(i);
      JsonObject obj = arr.add<JsonObject>();
      obj["on"] = st.powerOn;
      obj["bri"] = st.brightness;
      float briScale = st.powerOn ? (static_cast<float>(st.brightness) / 254.0f) : 0.0f;
      obj["r"] = static_cast<uint8_t>(st.red * briScale);
      obj["g"] = static_cast<uint8_t>(st.green * briScale);
      obj["b"] = static_cast<uint8_t>(st.blue * briScale);
      obj["w"] = static_cast<uint8_t>(st.white * briScale);
    }
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  });

  // WLED device discovery API
  server.on("/api/wled/discover", HTTP_GET, []() {
    std::vector<WledDeviceInfo> devices;
    wledDiscover(devices);

    JsonDocument doc;
    JsonArray arr = doc["devices"].to<JsonArray>();
    for (const auto& dev : devices) {
      JsonObject obj = arr.add<JsonObject>();
      obj["name"] = dev.name;
      obj["host"] = dev.host;
      obj["port"] = dev.port;
      obj["mac"] = dev.mac;
      obj["ledCount"] = dev.ledCount;
      obj["isRGBW"] = dev.isRGBW;
      obj["version"] = dev.version;
    }

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  });

  // Config API - GET
  server.on("/api/config", HTTP_GET, []() {
    JsonDocument doc;
    configStore.toJson(doc);

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  });

  // Config API - POST
  server.on("/api/config", HTTP_POST, []() {
    if (!server.hasArg("plain")) {
      server.send(400, "application/json", "{\"error\":\"No body\"}");
      return;
    }

    String body = server.arg("plain");
    ESP_LOGI("Web", "POST /api/config body (%d bytes): %s", body.length(), body.c_str());

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err) {
      ESP_LOGE("Web", "JSON parse error: %s", err.c_str());
      String resp = "{\"error\":\"Invalid JSON: ";
      resp += err.c_str();
      resp += "\"}";
      server.send(400, "application/json", resp);
      return;
    }

    if (configStore.fromJson(doc)) {
      configStore.save();
      ESP_LOGI("Web", "Config saved: %d lights", configStore.getLightCount());
      matterReconfigure();
      server.send(200, "application/json", "{\"ok\":true}");
    } else {
      ESP_LOGE("Web", "fromJson failed");
      server.send(400, "application/json", "{\"error\":\"Invalid config - 'lights' array not found\"}");
    }
  });

  // WiFi API - POST
  server.on("/api/wifi", HTTP_POST, []() {
    if (!server.hasArg("plain")) {
      server.send(400, "application/json", "{\"error\":\"No body\"}");
      return;
    }

    String body = server.arg("plain");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);

    if (err) {
      server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
      return;
    }

    String ssid = doc["ssid"] | "";
    String pass = doc["password"] | "";

    if (ssid.length() == 0) {
      server.send(400, "application/json", "{\"error\":\"SSID required\"}");
      return;
    }

    saveWifiCreds(ssid, pass);
    server.send(200, "application/json", "{\"ok\":true}");

    delay(1000);
    ESP.restart();
  });

  // Matter-only reset API (preserves WiFi + light config)
  server.on("/api/matter-reset", HTTP_POST, []() {
    server.send(200, "application/json", "{\"ok\":true}");
    delay(500);
    matterFactoryReset();  // calls esp_restart()
  });

  // Full factory reset API
  server.on("/api/factory-reset", HTTP_POST, []() {
    configStore.factoryReset();

    // Also clear WiFi credentials
    Preferences p;
    p.begin("mtwled_wifi", false);
    p.clear();
    p.end();

    server.send(200, "application/json", "{\"ok\":true}");

    delay(500);
    matterFactoryReset();  // also erases Matter NVS and restarts
  });

  // Restart API
  server.on("/api/restart", HTTP_POST, []() {
    server.send(200, "application/json", "{\"ok\":true}");
    delay(1000);
    ESP.restart();
  });

  // OTA firmware update endpoint
  server.on("/api/ota", HTTP_POST,
    // Response handler (called after upload completes)
    []() {
      if (Update.hasError()) {
        server.send(500, "text/plain", "OTA update failed");
      } else {
        server.send(200, "text/plain", "OK");
        delay(1000);
        ESP.restart();
      }
    },
    // Upload handler (called for each chunk)
    []() {
      HTTPUpload& upload = server.upload();

      if (upload.status == UPLOAD_FILE_START) {
        ESP_LOGI("OTA", "OTA upload start: %s (%u bytes)", upload.filename.c_str(), upload.totalSize);
        wledOutput.stop();

        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          ESP_LOGE("OTA", "Update.begin failed: %s", Update.errorString());
        }
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          ESP_LOGE("OTA", "Update.write failed: %s", Update.errorString());
        }
      } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
          ESP_LOGI("OTA", "OTA upload complete: %u bytes", upload.totalSize);
        } else {
          ESP_LOGE("OTA", "Update.end failed: %s", Update.errorString());
        }
      }
    }
  );

  // Captive portal redirect
  server.onNotFound([]() {
    if (apMode) {
      server.sendHeader("Location", "http://192.168.4.1/");
      server.send(302, "text/plain", "Redirecting to captive portal");
    } else {
      server.send(404, "text/plain", "Not found");
    }
  });
}

// ---- Public functions ----

void webSetup() {
  loadWifiCreds();

  if (wifiSSID.length() > 0) {
    // Try to connect to stored WiFi — STA only (no softAP).
    // Starting a softAP here causes WiFi mode toggling that conflicts with
    // Matter's WiFi driver and mDNS binding.
    ESP_LOGI("Web", "Connecting to WiFi: %s", wifiSSID.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());

    // Wait for connection (non-blocking, with timeout)
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(500);
    }

    if (WiFi.status() == WL_CONNECTED) {
      ESP_LOGI("Web", "WiFi connected: %s", WiFi.localIP().toString().c_str());
      apMode = false;
      webOnWifiConnected();
    } else {
      // Failed to connect — fall back to AP mode for configuration
      ESP_LOGW("Web", "WiFi connection failed, starting AP mode for setup");
      WiFi.mode(WIFI_AP);
      WiFi.softAP("MatterWLED-Setup", "");
      apMode = true;
    }
  } else {
    // No WiFi configured - start AP mode only
    ESP_LOGI("Web", "No WiFi configured, starting AP mode");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("MatterWLED-Setup", "");
    apMode = true;
  }

  if (apMode) {
    // Start DNS server for captive portal
    dnsServer.start(53, "*", WiFi.softAPIP());
  }

  setupRoutes();
  server.begin();
  serverStarted = true;

  ESP_LOGI("Web", "Web server started%s", apMode ? " (AP mode with captive portal)" : "");
}

void webLoop() {
  if (apMode) {
    dnsServer.processNextRequest();
  }

  // Process incoming HTTP requests
  server.handleClient();

  // Check if WiFi reconnected (use esp_netif-aware helper, not WiFi.status())
  static bool wasConnected = false;
  bool nowConnected = isWiFiConnected();
  if (!wasConnected && nowConnected) {
    wasConnected = true;
    if (apMode) {
      apMode = false;
      WiFi.softAPdisconnect(true);
    }
    ESP_LOGI("Web", "WiFi connected: %s", getStaIPString().c_str());
    webOnWifiConnected();
  } else if (wasConnected && !nowConnected) {
    wasConnected = false;
  }
}

void webOnWifiConnected() {
  // Start Matter stack once WiFi is up — Matter's EspDnssdInit() calls
  // mdns_init() internally, creating the shared ESP-IDF mDNS service.
  // (CONFIG_USE_MINIMAL_MDNS=n ensures CHIP uses the ESP-IDF mDNS stack
  // instead of its own "minimal mDNS", so both Matter advertising and
  // our WLED discovery share the same mDNS service.)
  matterOnWifiConnected();

  // Initialize mDNS for WLED discovery — since Matter already called
  // mdns_init(), this is a no-op for init but ensures our internal
  // ready flag is set and forces the STA PCB to be enabled for IPv4.
  wledDiscoveryInit();
}
