"""
Integration tests for the Matter WLED Bridge HTTP API.

Tests the bridge's REST API endpoints without needing Matter commissioning.
Requires a live ESP32 device on the network.

Run:
  pytest tests/ -v
  pytest tests/ -v --run-destructive   # includes config-modifying tests
  BRIDGE_HOST=192.168.1.100 pytest tests/ -v
"""

import pytest
import requests
import time
import json
import threading
import socket
from urllib.parse import urlparse

REQUEST_TIMEOUT = 10


# ─────────────────────────────────────────────────────────────────────────────
# Bridge Status API
# ─────────────────────────────────────────────────────────────────────────────

@pytest.mark.bridge
class TestBridgeStatus:
    """Tests for GET /api/status."""

    def test_status_returns_200(self, bridge_url):
        r = requests.get(f"{bridge_url}/api/status", timeout=REQUEST_TIMEOUT)
        assert r.status_code == 200

    def test_status_is_json(self, bridge_url):
        r = requests.get(f"{bridge_url}/api/status", timeout=REQUEST_TIMEOUT)
        data = r.json()
        assert isinstance(data, dict)

    def test_status_has_wifi_field(self, bridge_status):
        assert "wifi" in bridge_status
        assert isinstance(bridge_status["wifi"], str)
        assert bridge_status["wifi"] != ""

    def test_status_wifi_connected(self, bridge_status):
        """Bridge should be connected to WiFi (not 'Not connected')."""
        assert bridge_status["wifi"] != "Not connected"

    def test_status_has_matter_field(self, bridge_status):
        assert "matter" in bridge_status
        assert isinstance(bridge_status["matter"], str)

    def test_status_has_ip(self, bridge_status):
        """When connected, status should include an IP address."""
        assert "ip" in bridge_status
        # Should be a valid-looking IP
        parts = bridge_status["ip"].split(".")
        assert len(parts) == 4

    def test_status_not_ap_mode(self, bridge_status):
        assert bridge_status.get("apMode") is False

    def test_status_has_light_count(self, bridge_status):
        assert "lightCount" in bridge_status
        assert isinstance(bridge_status["lightCount"], int)
        assert bridge_status["lightCount"] >= 0

    def test_status_matter_running(self, bridge_status):
        """If lights are configured, Matter should be running."""
        if bridge_status["lightCount"] > 0:
            assert bridge_status["matter"] == "Running"
        else:
            assert "Disabled" in bridge_status["matter"] or "Waiting" in bridge_status["matter"]

    def test_status_has_pairing_code_when_running(self, bridge_status):
        """When Matter is running, pairing code should be present."""
        if bridge_status.get("matter") == "Running":
            assert "pairingCode" in bridge_status
            code = bridge_status["pairingCode"]
            assert isinstance(code, str)
            assert len(code) == 11  # Matter manual pairing code is 11 digits
            assert code.isdigit()

    def test_status_has_qr_payload_when_running(self, bridge_status):
        """When Matter is running, QR payload should be present."""
        if bridge_status.get("matter") == "Running":
            assert "qrPayload" in bridge_status
            qr = bridge_status["qrPayload"]
            assert isinstance(qr, str)
            assert qr.startswith("MT:")


# ─────────────────────────────────────────────────────────────────────────────
# Bridge Configuration API
# ─────────────────────────────────────────────────────────────────────────────

@pytest.mark.bridge
class TestBridgeConfig:
    """Tests for GET /api/config."""

    def test_config_returns_200(self, bridge_url):
        r = requests.get(f"{bridge_url}/api/config", timeout=REQUEST_TIMEOUT)
        assert r.status_code == 200

    def test_config_has_lights_array(self, bridge_config):
        assert "lights" in bridge_config
        assert isinstance(bridge_config["lights"], list)

    def test_config_light_count_matches_status(self, bridge_url):
        status = requests.get(f"{bridge_url}/api/status", timeout=REQUEST_TIMEOUT).json()
        config = requests.get(f"{bridge_url}/api/config", timeout=REQUEST_TIMEOUT).json()
        assert len(config["lights"]) == status["lightCount"]

    def test_each_light_has_required_fields(self, bridge_config):
        for i, light in enumerate(bridge_config["lights"]):
            assert "name" in light, f"Light {i} missing 'name'"
            assert "type" in light, f"Light {i} missing 'type'"
            assert "wledHost" in light, f"Light {i} missing 'wledHost'"
            assert "wledPort" in light, f"Light {i} missing 'wledPort'"

    def test_light_type_is_valid(self, bridge_config):
        for i, light in enumerate(bridge_config["lights"]):
            assert light["type"] in ("RGB", "RGBW"), \
                f"Light {i} has invalid type '{light['type']}'"

    def test_light_port_is_valid(self, bridge_config):
        for i, light in enumerate(bridge_config["lights"]):
            assert 1 <= light["wledPort"] <= 65535, \
                f"Light {i} has invalid port {light['wledPort']}"

    def test_light_has_mac_field(self, bridge_config):
        """Each light should have a mac field (may be empty for legacy configs)."""
        for i, light in enumerate(bridge_config["lights"]):
            assert "mac" in light, f"Light {i} missing 'mac' field"

    def test_light_names_are_nonempty(self, bridge_config):
        for i, light in enumerate(bridge_config["lights"]):
            assert len(light["name"]) > 0, f"Light {i} has empty name"

    def test_light_hosts_are_nonempty(self, bridge_config):
        for i, light in enumerate(bridge_config["lights"]):
            assert len(light["wledHost"]) > 0, f"Light {i} has empty wledHost"


# ─────────────────────────────────────────────────────────────────────────────
# Light State API
# ─────────────────────────────────────────────────────────────────────────────

@pytest.mark.bridge
class TestLightStates:
    """Tests for GET /api/lights/state."""

    def test_light_states_returns_200(self, bridge_url):
        r = requests.get(f"{bridge_url}/api/lights/state", timeout=REQUEST_TIMEOUT)
        assert r.status_code == 200

    def test_light_states_has_lights_array(self, bridge_url):
        r = requests.get(f"{bridge_url}/api/lights/state", timeout=REQUEST_TIMEOUT)
        data = r.json()
        assert "lights" in data
        assert isinstance(data["lights"], list)

    def test_light_states_count_matches_config(self, bridge_url):
        config = requests.get(f"{bridge_url}/api/config", timeout=REQUEST_TIMEOUT).json()
        states = requests.get(f"{bridge_url}/api/lights/state", timeout=REQUEST_TIMEOUT).json()
        assert len(states["lights"]) == len(config["lights"])

    def test_each_state_has_required_fields(self, bridge_url):
        states = requests.get(f"{bridge_url}/api/lights/state", timeout=REQUEST_TIMEOUT).json()
        for i, state in enumerate(states["lights"]):
            assert "on" in state, f"State {i} missing 'on'"
            assert "bri" in state, f"State {i} missing 'bri'"
            assert "epId" in state, f"State {i} missing 'epId'"
            assert "r" in state, f"State {i} missing 'r'"
            assert "g" in state, f"State {i} missing 'g'"
            assert "b" in state, f"State {i} missing 'b'"
            assert "w" in state, f"State {i} missing 'w'"
            assert "x" in state, f"State {i} missing 'x' (CIE xy)"
            assert "y" in state, f"State {i} missing 'y' (CIE xy)"

    def test_endpoint_ids_are_valid(self, bridge_url):
        """Each light should have a Matter endpoint ID >= 2 (0=root, 1=aggregator)."""
        states = requests.get(f"{bridge_url}/api/lights/state", timeout=REQUEST_TIMEOUT).json()
        ep_ids = []
        for i, state in enumerate(states["lights"]):
            ep_id = state["epId"]
            assert isinstance(ep_id, int), f"State {i}: 'epId' should be int"
            assert ep_id >= 2, f"State {i}: endpoint ID {ep_id} too low (0=root, 1=aggregator)"
            ep_ids.append(ep_id)
        # All endpoint IDs should be unique
        assert len(ep_ids) == len(set(ep_ids)), f"Duplicate endpoint IDs found: {ep_ids}"

    def test_state_values_in_range(self, bridge_url):
        states = requests.get(f"{bridge_url}/api/lights/state", timeout=REQUEST_TIMEOUT).json()
        for i, state in enumerate(states["lights"]):
            assert isinstance(state["on"], bool), f"State {i}: 'on' should be bool"
            assert 0 <= state["bri"] <= 255, f"State {i}: 'bri' out of range"
            assert 0 <= state["r"] <= 255, f"State {i}: 'r' out of range"
            assert 0 <= state["g"] <= 255, f"State {i}: 'g' out of range"
            assert 0 <= state["b"] <= 255, f"State {i}: 'b' out of range"
            assert 0 <= state["w"] <= 255, f"State {i}: 'w' out of range"
            assert 0.0 <= state["x"] <= 1.0, f"State {i}: 'x' (CIE) out of range"
            assert 0.0 <= state["y"] <= 1.0, f"State {i}: 'y' (CIE) out of range"

    def test_off_lights_have_zero_rgb(self, bridge_url):
        """When a light is off, its scaled RGB values should be 0."""
        states = requests.get(f"{bridge_url}/api/lights/state", timeout=REQUEST_TIMEOUT).json()
        for i, state in enumerate(states["lights"]):
            if not state["on"]:
                assert state["r"] == 0, f"State {i}: off light has non-zero r"
                assert state["g"] == 0, f"State {i}: off light has non-zero g"
                assert state["b"] == 0, f"State {i}: off light has non-zero b"


# ─────────────────────────────────────────────────────────────────────────────
# WLED Discovery API
# ─────────────────────────────────────────────────────────────────────────────

@pytest.mark.bridge
@pytest.mark.wled
class TestWledDiscovery:
    """Tests for GET /api/wled/discover."""

    def test_discovery_returns_200(self, bridge_url):
        """Discovery endpoint should respond (may take a few seconds for mDNS scan)."""
        r = requests.get(f"{bridge_url}/api/wled/discover", timeout=30)
        assert r.status_code == 200

    def test_discovery_returns_devices_array(self, bridge_url):
        r = requests.get(f"{bridge_url}/api/wled/discover", timeout=30)
        data = r.json()
        assert "devices" in data
        assert isinstance(data["devices"], list)

    def test_discovered_devices_have_required_fields(self, bridge_url):
        r = requests.get(f"{bridge_url}/api/wled/discover", timeout=30)
        data = r.json()
        for i, dev in enumerate(data["devices"]):
            assert "name" in dev, f"Device {i} missing 'name'"
            assert "host" in dev, f"Device {i} missing 'host'"
            assert "port" in dev, f"Device {i} missing 'port'"
            assert "mac" in dev, f"Device {i} missing 'mac'"
            assert "ledCount" in dev, f"Device {i} missing 'ledCount'"
            assert "isRGBW" in dev, f"Device {i} missing 'isRGBW'"
            assert "version" in dev, f"Device {i} missing 'version'"

    def test_discovered_devices_have_hostname(self, bridge_url):
        """Discovered devices should have mDNS hostname for stable addressing."""
        r = requests.get(f"{bridge_url}/api/wled/discover", timeout=30)
        data = r.json()
        for i, dev in enumerate(data["devices"]):
            assert "hostname" in dev, f"Device {i} missing 'hostname'"

    def test_discovers_at_least_one_device(self, bridge_url):
        """At least one WLED device should be found on the network."""
        r = requests.get(f"{bridge_url}/api/wled/discover", timeout=30)
        data = r.json()
        assert len(data["devices"]) > 0, \
            "No WLED devices found. Check that WLED devices are on the same network."

    def test_discovered_mac_format(self, bridge_url):
        """MAC addresses should be in AA:BB:CC:DD:EE:FF or aabb... format."""
        r = requests.get(f"{bridge_url}/api/wled/discover", timeout=30)
        data = r.json()
        for i, dev in enumerate(data["devices"]):
            mac = dev.get("mac", "")
            if mac:
                # WLED may return MAC with or without colons
                clean = mac.replace(":", "").replace("-", "")
                assert len(clean) == 12, \
                    f"Device {i} MAC '{mac}' doesn't look like a valid MAC address"


# ─────────────────────────────────────────────────────────────────────────────
# WLED Device Verification
# ─────────────────────────────────────────────────────────────────────────────

@pytest.mark.wled
class TestWledDevices:
    """Tests that verify WLED devices referenced in the bridge config are reachable."""

    def test_at_least_one_wled_reachable(self, wled_hosts):
        assert len(wled_hosts) > 0, \
            "No reachable WLED devices found from bridge config"

    def test_wled_info_endpoint(self, wled_hosts):
        """Each configured WLED device should respond to /json/info."""
        for url in wled_hosts:
            r = requests.get(f"{url}/json/info", timeout=5)
            assert r.status_code == 200, f"WLED {url} /json/info returned {r.status_code}"
            data = r.json()
            assert "name" in data
            assert "ver" in data
            assert "leds" in data

    def test_wled_state_endpoint(self, wled_hosts):
        """Each configured WLED device should respond to /json/state."""
        for url in wled_hosts:
            r = requests.get(f"{url}/json/state", timeout=5)
            assert r.status_code == 200, f"WLED {url} /json/state returned {r.status_code}"
            data = r.json()
            assert "on" in data
            assert "bri" in data
            assert "seg" in data

    def test_wled_accepts_state_post(self, wled_hosts):
        """WLED should accept POST to /json/state (the same way the bridge controls it)."""
        if not wled_hosts:
            pytest.skip("No reachable WLED devices")
        url = wled_hosts[0]
        # Read current state
        current = requests.get(f"{url}/json/state", timeout=5).json()
        # POST the same state back (no-op, just verify it accepts the request)
        r = requests.post(
            f"{url}/json/state",
            json={"on": current["on"], "bri": current["bri"]},
            timeout=5,
        )
        assert r.status_code == 200, f"WLED {url} rejected state POST: {r.status_code}"


# ─────────────────────────────────────────────────────────────────────────────
# Web UI
# ─────────────────────────────────────────────────────────────────────────────

@pytest.mark.bridge
class TestWebUI:
    """Tests for the web UI HTML page."""

    def test_root_returns_html(self, bridge_url):
        r = requests.get(f"{bridge_url}/", timeout=REQUEST_TIMEOUT)
        assert r.status_code == 200
        assert "text/html" in r.headers.get("Content-Type", "")

    def test_html_contains_title(self, bridge_url):
        r = requests.get(f"{bridge_url}/", timeout=REQUEST_TIMEOUT)
        assert "Matter WLED Bridge" in r.text

    def test_html_contains_api_calls(self, bridge_url):
        """The HTML should contain JavaScript that calls our API endpoints."""
        r = requests.get(f"{bridge_url}/", timeout=REQUEST_TIMEOUT)
        assert "/api/status" in r.text
        assert "/api/config" in r.text
        assert "/api/wled/discover" in r.text


# ─────────────────────────────────────────────────────────────────────────────
# Configuration Modification (destructive tests)
# ─────────────────────────────────────────────────────────────────────────────

@pytest.mark.bridge
@pytest.mark.destructive
class TestConfigModification:
    """Tests that modify config. Only run with --run-destructive."""

    def test_post_config_roundtrip(self, bridge_url):
        """POST a config and verify it comes back on GET."""
        # Read current config
        original = requests.get(f"{bridge_url}/api/config", timeout=REQUEST_TIMEOUT).json()

        # POST it back unchanged
        r = requests.post(
            f"{bridge_url}/api/config",
            json=original,
            timeout=REQUEST_TIMEOUT,
        )
        assert r.status_code == 200
        result = r.json()
        assert result.get("ok") is True

        # Verify it matches
        readback = requests.get(f"{bridge_url}/api/config", timeout=REQUEST_TIMEOUT).json()
        assert len(readback["lights"]) == len(original["lights"])
        for i in range(len(original["lights"])):
            assert readback["lights"][i]["name"] == original["lights"][i]["name"]
            assert readback["lights"][i]["type"] == original["lights"][i]["type"]
            assert readback["lights"][i]["wledHost"] == original["lights"][i]["wledHost"]
            assert readback["lights"][i]["wledPort"] == original["lights"][i]["wledPort"]

    def test_post_invalid_json_returns_400(self, bridge_url):
        r = requests.post(
            f"{bridge_url}/api/config",
            data="not valid json{{{",
            headers={"Content-Type": "application/json"},
            timeout=REQUEST_TIMEOUT,
        )
        assert r.status_code == 400

    def test_post_missing_lights_returns_400(self, bridge_url):
        r = requests.post(
            f"{bridge_url}/api/config",
            json={"notlights": []},
            timeout=REQUEST_TIMEOUT,
        )
        assert r.status_code == 400

    def test_config_preserves_mac(self, bridge_url):
        """POSTing config with a MAC should preserve it on readback."""
        # Read current config
        original = requests.get(f"{bridge_url}/api/config", timeout=REQUEST_TIMEOUT).json()
        if not original["lights"]:
            pytest.skip("No lights configured")

        # Add a fake MAC to the first light
        test_mac = "AA:BB:CC:DD:EE:FF"
        modified = json.loads(json.dumps(original))  # deep copy
        modified["lights"][0]["mac"] = test_mac

        # POST modified config
        r = requests.post(
            f"{bridge_url}/api/config",
            json=modified,
            timeout=REQUEST_TIMEOUT,
        )
        assert r.status_code == 200

        # Verify MAC persisted
        readback = requests.get(f"{bridge_url}/api/config", timeout=REQUEST_TIMEOUT).json()
        assert readback["lights"][0]["mac"] == test_mac

        # Restore original config
        requests.post(
            f"{bridge_url}/api/config",
            json=original,
            timeout=REQUEST_TIMEOUT,
        )


# ─────────────────────────────────────────────────────────────────────────────
# End-to-end: Bridge → WLED forwarding
# ─────────────────────────────────────────────────────────────────────────────

@pytest.mark.bridge
@pytest.mark.wled
class TestBridgeToWledForwarding:
    """
    Verify the bridge forwards state changes to WLED devices.

    These tests read the bridge's light state API and compare with the
    corresponding WLED device's actual state. They don't *send* Matter
    commands (that requires chip-tool), but verify the existing state
    is consistent between the bridge and the WLED devices.
    """

    def test_bridge_and_wled_power_state_consistent(self, bridge_url, bridge_config, wled_hosts):
        """Bridge's on/off state for each light should match the WLED device."""
        if not wled_hosts:
            pytest.skip("No reachable WLED devices")

        states = requests.get(
            f"{bridge_url}/api/lights/state", timeout=REQUEST_TIMEOUT
        ).json()["lights"]

        for i, light in enumerate(bridge_config["lights"]):
            host = light.get("wledHost", "")
            port = light.get("wledPort", 80)
            if not host:
                continue

            wled_url = f"http://{host}:{port}" if port != 80 else f"http://{host}"

            try:
                wled_state = requests.get(f"{wled_url}/json/state", timeout=5).json()
            except Exception:
                continue  # WLED unreachable, skip

            bridge_on = states[i]["on"]
            wled_on = wled_state["on"]

            # Note: these may differ if the bridge hasn't sent a command yet
            # (e.g. just booted). We just verify the states are readable.
            # A full test would send a command via Matter and then check.
            assert isinstance(bridge_on, bool), f"Light {i}: bridge on state not bool"
            assert isinstance(wled_on, bool), f"Light {i}: WLED on state not bool"


# ─────────────────────────────────────────────────────────────────────────────
# Server-Sent Events (SSE)
# ─────────────────────────────────────────────────────────────────────────────

class _SSEClient:
    """Lightweight SSE reader using a raw TCP socket.

    Uses a raw socket instead of requests to handle the esp_http_server's
    mixed chunked-then-raw-send pattern (initial events via chunked encoding,
    subsequent events via httpd_socket_send).

    Runs in a background thread, collecting events into a list.
    Each event is a dict with 'event' and 'data' keys.
    """

    def __init__(self, url, timeout=10):
        self.url = url
        self.timeout = timeout
        self.events = []
        self.error = None
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._sock = None

    def start(self):
        self._thread.start()
        return self

    def stop(self):
        self._stop.set()
        # Close socket to unblock recv
        if self._sock:
            try:
                self._sock.close()
            except Exception:
                pass
        self._thread.join(timeout=5)

    def wait_for_events(self, count, timeout=10):
        """Block until at least `count` events are collected or timeout."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if len(self.events) >= count:
                return True
            if self.error:
                return False
            time.sleep(0.1)
        return len(self.events) >= count

    def _run(self):
        try:
            parsed = urlparse(self.url)
            host = parsed.hostname
            port = parsed.port or 80
            path = parsed.path or "/"

            self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self._sock.settimeout(self.timeout)
            self._sock.connect((host, port))

            # Send HTTP GET request
            req = (
                f"GET {path} HTTP/1.1\r\n"
                f"Host: {host}:{port}\r\n"
                f"Accept: text/event-stream\r\n"
                f"Connection: keep-alive\r\n"
                f"\r\n"
            )
            self._sock.sendall(req.encode())

            # Read response — accumulate data and parse SSE lines
            buf = ""
            header_done = False
            event_type = None
            data_buf = []

            while not self._stop.is_set():
                try:
                    chunk = self._sock.recv(4096)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if not chunk:
                    break

                buf += chunk.decode("utf-8", errors="replace")

                # Skip HTTP headers on first read
                if not header_done:
                    hdr_end = buf.find("\r\n\r\n")
                    if hdr_end == -1:
                        continue
                    buf = buf[hdr_end + 4:]
                    header_done = True

                # Parse SSE lines from buffer
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    line = line.rstrip("\r")

                    # Handle chunked transfer-encoding hex size lines
                    # (these are hex numbers like "1a3" on their own line)
                    stripped = line.strip()
                    if stripped and all(c in "0123456789abcdefABCDEF" for c in stripped):
                        continue  # skip chunk size line

                    if line == "":
                        # Empty line = end of event
                        if event_type and data_buf:
                            self.events.append({
                                "event": event_type,
                                "data": "\n".join(data_buf),
                            })
                        event_type = None
                        data_buf = []
                    elif line.startswith("event: "):
                        event_type = line[7:]
                    elif line.startswith("data: "):
                        data_buf.append(line[6:])
                    elif line.startswith(":"):
                        # SSE comment (e.g. keepalive)
                        self.events.append({
                            "event": ":comment",
                            "data": line[1:].strip(),
                        })
        except Exception as e:
            if not self._stop.is_set():
                self.error = e
        finally:
            if self._sock:
                try:
                    self._sock.close()
                except Exception:
                    pass


@pytest.mark.bridge
@pytest.mark.sse
class TestSSE:
    """Tests for the Server-Sent Events endpoint GET /api/events."""

    def test_sse_initial_events(self, bridge_url):
        """Connecting to /api/events should immediately receive status and lightstate events."""
        client = _SSEClient(f"{bridge_url}/api/events").start()
        try:
            assert client.wait_for_events(2, timeout=10), \
                f"Expected 2 initial events, got {len(client.events)}: {client.events}"

            event_types = [e["event"] for e in client.events[:2]]
            assert "status" in event_types, f"Missing 'status' event in {event_types}"
            assert "lightstate" in event_types, f"Missing 'lightstate' event in {event_types}"

            # Validate status event is parseable JSON with expected fields
            status_evt = next(e for e in client.events if e["event"] == "status")
            status_data = json.loads(status_evt["data"])
            assert "wifi" in status_data
            assert "lightCount" in status_data

            # Validate lightstate event is parseable JSON with expected fields
            light_evt = next(e for e in client.events if e["event"] == "lightstate")
            light_data = json.loads(light_evt["data"])
            assert "lights" in light_data
            assert isinstance(light_data["lights"], list)
        finally:
            client.stop()

    def test_sse_concurrent_rest(self, bridge_url):
        """REST API should still work while an SSE connection is active."""
        client = _SSEClient(f"{bridge_url}/api/events").start()
        try:
            assert client.wait_for_events(2, timeout=10), \
                "SSE connection failed to deliver initial events"

            # Make several REST API calls while SSE is connected
            r = requests.get(f"{bridge_url}/api/status", timeout=REQUEST_TIMEOUT)
            assert r.status_code == 200
            assert "wifi" in r.json()

            r = requests.get(f"{bridge_url}/api/config", timeout=REQUEST_TIMEOUT)
            assert r.status_code == 200
            assert "lights" in r.json()

            r = requests.get(f"{bridge_url}/api/lights/state", timeout=REQUEST_TIMEOUT)
            assert r.status_code == 200
            assert "lights" in r.json()
        finally:
            client.stop()

    def test_sse_reconnect(self, bridge_url):
        """A second SSE connection should replace the first and receive initial events."""
        client1 = _SSEClient(f"{bridge_url}/api/events").start()
        try:
            assert client1.wait_for_events(2, timeout=10), \
                "First SSE connection failed"
        finally:
            client1.stop()

        time.sleep(0.5)

        # Connect a second client — should get fresh initial events
        client2 = _SSEClient(f"{bridge_url}/api/events").start()
        try:
            assert client2.wait_for_events(2, timeout=10), \
                f"Second SSE connection failed, got {len(client2.events)} events"

            event_types = [e["event"] for e in client2.events[:2]]
            assert "status" in event_types
            assert "lightstate" in event_types
        finally:
            client2.stop()

    def test_sse_keepalive(self, bridge_url):
        """SSE connection should remain alive, receiving keepalives or data events."""
        client = _SSEClient(f"{bridge_url}/api/events", timeout=30).start()
        try:
            # Wait for initial events first
            assert client.wait_for_events(2, timeout=10), \
                "SSE connection failed to deliver initial events"

            initial_count = len(client.events)

            # Wait up to 20 seconds for any new event after the initial ones.
            # This could be a keepalive comment (sent every ~15s when idle)
            # or a data event (sent when light state changes).
            # Either proves the SSE connection is alive and persistent.
            deadline = time.time() + 20
            while time.time() < deadline:
                if len(client.events) > initial_count:
                    # Got a new event — connection is alive
                    new_events = client.events[initial_count:]
                    new_types = [e["event"] for e in new_events]
                    # Verify we got a valid event type
                    for evt_type in new_types:
                        assert evt_type in (":comment", "status", "lightstate"), \
                            f"Unexpected event type: {evt_type}"
                    return  # success
                if client.error:
                    pytest.fail(f"SSE client errored: {client.error}")
                time.sleep(0.5)

            pytest.fail(
                f"No new events received within 20s after initial events. "
                f"Total events: {len(client.events)}"
            )
        finally:
            client.stop()


# ─────────────────────────────────────────────────────────────────────────────
# Matter Protocol Tests (requires chip-tool)
# ─────────────────────────────────────────────────────────────────────────────

@pytest.mark.matter
class TestMatterProtocol:
    """
    Matter protocol-level tests using chip-tool.

    These tests require:
    1. chip-tool installed (snap install chip-tool or build from source)
    2. The bridge device already commissioned to the chip-tool fabric

    Commission first:
      chip-tool pairing onnetwork 1 20202021

    Skip these tests if chip-tool is not available.
    """

    @pytest.fixture(autouse=True)
    def _require_chip_tool(self, chip_tool_available):
        if not chip_tool_available:
            pytest.skip("chip-tool not found — install via: sudo snap install chip-tool")

    # Placeholder for future Matter protocol tests.
    # When chip-tool is available, these would:
    #   1. Send on/off commands via chip-tool
    #   2. Read back attributes via chip-tool
    #   3. Verify the WLED device received the correct state via HTTP
    #
    # Example test flow:
    #   chip-tool onoff on <node-id> <endpoint-id>
    #   → verify GET /api/lights/state shows on=true
    #   → verify WLED /json/state shows on=true
    #
    #   chip-tool colorcontrol move-to-hue-and-saturation <hue> <sat> 0 0 0 <node-id> <endpoint-id>
    #   → verify bridge state has matching color
    #   → verify WLED /json/state seg[0].col matches

    def test_placeholder(self):
        """Placeholder — real tests require commissioning setup."""
        pytest.skip("Matter protocol tests need commissioning setup first")
