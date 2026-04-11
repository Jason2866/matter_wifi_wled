Import("env")
import shutil
from pathlib import Path
from SCons.Script import Exit

# ── setup_matter_component.py ─────────────────────────────────────────────────
# Pre-build script for Matter WiFi WLED Bridge.
#
# 1. Copies the idf_component.yml manifest into src/ so that pioarduino's
#    built-in ComponentManager (and the IDF Component Manager) can resolve
#    the espressif/esp_matter dependency.
#
# 2. Patches TypeTraits.h in the CHIP SDK to fix a broken C++23 branch
#    that uses invalid syntax: `using to_underlying = std::to_underlying;`
#    (you cannot type-alias a function template; correct form is
#    `using std::to_underlying;`). This bug triggers with GCC 14 + gnu++2b.
#
# The destination file is listed in .gitignore and must NOT be committed.
# ─────────────────────────────────────────────────────────────────────────────

project_dir = Path(env["PROJECT_DIR"]).resolve()

# ── Step 1: Copy idf_component.yml ───────────────────────────────────────────
src_yml = project_dir / "idf_component.yml"
dst_yml = project_dir / "src" / "idf_component.yml"

if not src_yml.exists():
    print(
        "\033[0;31;43m"
        f"Matter: idf_component.yml not found at {src_yml} – "
        "cannot resolve the esp_matter component."
        "\033[0m"
    )
    Exit(1)

shutil.copy2(str(src_yml), str(dst_yml))
print(
    "\033[6;33;42m"
    "Matter: copied idf_component.yml → src/idf_component.yml"
    "\033[0m"
)

# ── Step 2: Patch TypeTraits.h C++23 bug ─────────────────────────────────────
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

BROKEN_LINE  = "using to_underlying = std::to_underlying;"
FIXED_LINE   = "using std::to_underlying;"

if type_traits_h.exists():
    content = type_traits_h.read_text()
    if BROKEN_LINE in content:
        content = content.replace(BROKEN_LINE, FIXED_LINE)
        type_traits_h.write_text(content)
        print(
            "\033[6;33;42m"
            "Matter: patched TypeTraits.h — fixed C++23 to_underlying syntax"
            "\033[0m"
        )
    else:
        print("Matter: TypeTraits.h already patched or doesn't need patching")
else:
    # File doesn't exist yet during first CMake configuration pass;
    # it will exist on the next build invocation after component download.
    print("Matter: TypeTraits.h not found yet (will patch on next build)")
