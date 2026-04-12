"""
Pytest configuration and shared fixtures for Matter WLED Bridge integration tests.

These tests run against a live ESP32 device on the network. Configure the
device address via environment variables or the defaults below.

Environment variables:
  BRIDGE_HOST   — IP or hostname of the Matter WLED Bridge (default: 192.168.178.212)
  WLED_HOSTS    — Comma-separated WLED device IPs for verification (default: auto-detect from bridge config)
  CHIP_TOOL     — Path to chip-tool binary (optional, for Matter protocol tests)
"""

import os
import pytest
import requests
import time

# ── Configuration ────────────────────────────────────────────────────────────

BRIDGE_HOST = os.environ.get("BRIDGE_HOST", "192.168.178.212")
BRIDGE_URL = f"http://{BRIDGE_HOST}"
CHIP_TOOL = os.environ.get("CHIP_TOOL", "chip-tool")
REQUEST_TIMEOUT = 10  # seconds


def pytest_configure(config):
    """Register custom markers."""
    config.addinivalue_line("markers", "bridge: tests that only need the bridge HTTP API")
    config.addinivalue_line("markers", "wled: tests that also query WLED devices directly")
    config.addinivalue_line("markers", "matter: tests that require chip-tool (skipped if unavailable)")
    config.addinivalue_line("markers", "destructive: tests that modify device config (require --run-destructive)")
    config.addinivalue_line("markers", "sse: tests for the Server-Sent Events endpoint")


def pytest_addoption(parser):
    """Add custom CLI options."""
    parser.addoption(
        "--run-destructive",
        action="store_true",
        default=False,
        help="Run tests that modify device configuration (restart, config changes, etc.)",
    )


def pytest_collection_modifyitems(config, items):
    """Skip destructive tests unless --run-destructive is passed."""
    if not config.getoption("--run-destructive"):
        skip = pytest.mark.skip(reason="needs --run-destructive option to run")
        for item in items:
            if "destructive" in item.keywords:
                item.add_marker(skip)


# ── Fixtures ─────────────────────────────────────────────────────────────────

@pytest.fixture(scope="session")
def bridge_url():
    """Base URL of the Matter WLED Bridge."""
    return BRIDGE_URL


@pytest.fixture(scope="session")
def bridge_status(bridge_url):
    """Fetch and cache the bridge status. Fails fast if bridge is unreachable."""
    try:
        r = requests.get(f"{bridge_url}/api/status", timeout=REQUEST_TIMEOUT)
        r.raise_for_status()
        return r.json()
    except requests.exceptions.ConnectionError:
        pytest.fail(
            f"Cannot reach bridge at {bridge_url}. "
            f"Set BRIDGE_HOST env var or check the device is online."
        )


@pytest.fixture(scope="session")
def bridge_config(bridge_url):
    """Fetch and cache the bridge light configuration."""
    r = requests.get(f"{bridge_url}/api/config", timeout=REQUEST_TIMEOUT)
    r.raise_for_status()
    return r.json()


@pytest.fixture(scope="session")
def wled_hosts(bridge_config):
    """
    List of WLED device base URLs, derived from the bridge config.
    Filters to only devices reachable over HTTP.
    """
    env_hosts = os.environ.get("WLED_HOSTS")
    if env_hosts:
        return [f"http://{h.strip()}" for h in env_hosts.split(",")]

    hosts = []
    for light in bridge_config.get("lights", []):
        host = light.get("wledHost", "")
        port = light.get("wledPort", 80)
        if host:
            url = f"http://{host}:{port}" if port != 80 else f"http://{host}"
            # Verify reachable
            try:
                requests.get(f"{url}/json/info", timeout=3)
                hosts.append(url)
            except Exception:
                pass
    return hosts


@pytest.fixture(scope="session")
def chip_tool_available():
    """Check if chip-tool is available on PATH."""
    import shutil
    return shutil.which(CHIP_TOOL) is not None
