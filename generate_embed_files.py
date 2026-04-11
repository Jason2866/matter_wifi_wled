Import("env")
from pathlib import Path

# ── generate_embed_files.py ───────────────────────────────────────────────────
# Post-build script for the Matter WiFi WLED Bridge.
#
# Generates .S assembly files for binary data that ESP-IDF components
# expect to be embedded via CMake's target_add_binary_data().
# PlatformIO's SCons build doesn't execute those CMake commands,
# so we replicate the output here.
#
# This script MUST run as a post: script (not pre:) so that the IDF
# Component Manager has already downloaded managed_components/ during the
# CMake configuration phase.  The .S files are generated before SCons
# begins actual compilation, so they are available when needed.
#
# Based on the equivalent script from WLED-MM:
# https://github.com/netmindz/WLED-MM/blob/matter-over-wifi/usermods/matter_over_wifi/generate_embed_files.py
# ─────────────────────────────────────────────────────────────────────────────

project_dir = Path(env["PROJECT_DIR"]).resolve()
build_dir   = Path(env.subst("$BUILD_DIR")).resolve()

# Map of (source cert path, symbol name) pairs that need embedding.
# These correspond to all target_add_binary_data() calls in the managed
# components' CMakeLists.txt files (which PlatformIO's SCons build skips).
EMBED_FILES = [
    (
        project_dir / "managed_components" / "espressif__esp_insights" / "server_certs" / "https_server.crt",
        "https_server_crt",
    ),
    (
        project_dir / "managed_components" / "espressif__esp_insights" / "server_certs" / "mqtt_server.crt",
        "mqtt_server_crt",
    ),
    (
        project_dir / "managed_components" / "espressif__esp_rainmaker" / "server_certs" / "rmaker_mqtt_server.crt",
        "rmaker_mqtt_server_crt",
    ),
    (
        project_dir / "managed_components" / "espressif__esp_rainmaker" / "server_certs" / "rmaker_claim_service_server.crt",
        "rmaker_claim_service_server_crt",
    ),
    (
        project_dir / "managed_components" / "espressif__esp_rainmaker" / "server_certs" / "rmaker_ota_server.crt",
        "rmaker_ota_server_crt",
    ),
]


def generate_asm(src_path: Path, symbol: str, out_dir: Path):
    """Generate an assembly .S file that embeds binary data, matching the
    format produced by ESP-IDF's ``target_add_binary_data()``."""
    if not src_path.exists():
        from SCons.Script import Exit
        print(f"  [embed] ERROR: {src_path} not found.")
        print("  [embed] The IDF Component Manager should have downloaded this file")
        print("  [embed] during the CMake configuration phase.  Make sure this script")
        print("  [embed] runs as a post: script (not pre:) in platformio.ini.")
        Exit(1)

    out_file = out_dir / f"{src_path.name}.S"
    data = src_path.read_bytes()

    lines = [
        f"/* Data converted from {src_path} */",
        ".data",
        "#if !defined (__APPLE__) && !defined (__linux__)",
        ".section .rodata.embedded",
        "#endif",
        "",
        f".global {symbol}",
        f"{symbol}:",
        "",
        f".global _binary_{symbol}_start",
        f"_binary_{symbol}_start: /* for objcopy compatibility */",
    ]

    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        hexvals = ", ".join(f"0x{b:02x}" for b in chunk)
        lines.append(f".byte {hexvals}")

    lines += [
        "",
        f".global _binary_{symbol}_end",
        f"_binary_{symbol}_end: /* for objcopy compatibility */",
        ".byte 0x00",  # null terminator for TEXT mode
    ]

    out_dir.mkdir(parents=True, exist_ok=True)
    out_file.write_text("\n".join(lines) + "\n")
    print(f"  [embed] Generated {out_file} ({len(data)} bytes)")
    return str(out_file)


for src_path, symbol in EMBED_FILES:
    generate_asm(src_path, symbol, build_dir)


# ── Patch TypeTraits.h C++23 bug ─────────────────────────────────────────────
# This MUST run as a post-script (after CMake configuration downloads
# managed_components).  The pre-script can't reliably patch it because
# the IDF Component Manager may re-download the component afterward.
#
# Bug: line 37 of TypeTraits.h has:
#   using to_underlying = std::to_underlying;
# which is syntactically invalid C++ (can't type-alias a function template).
# The correct form is a using-declaration:
#   using std::to_underlying;
# ─────────────────────────────────────────────────────────────────────────────

type_traits_h = (
    project_dir
    / "managed_components"
    / "espressif__esp_matter"
    / "connectedhomeip"
    / "connectedhomeip"
    / "src"
    / "lib"
    / "support"
    / "TypeTraits.h"
)

BROKEN_LINE = "using to_underlying = std::to_underlying;"
FIXED_LINE  = "using std::to_underlying;"

if type_traits_h.exists():
    content = type_traits_h.read_text()
    if BROKEN_LINE in content:
        content = content.replace(BROKEN_LINE, FIXED_LINE)
        type_traits_h.write_text(content)
        print("  [patch] TypeTraits.h — fixed C++23 to_underlying syntax")
    else:
        print("  [patch] TypeTraits.h — already patched")
else:
    print("  [patch] WARNING: TypeTraits.h not found — component may not be downloaded yet")
