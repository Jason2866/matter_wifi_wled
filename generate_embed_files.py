Import("env")
from pathlib import Path

project_dir = Path(env["PROJECT_DIR"]).resolve()
build_dir   = Path(env.subst("$BUILD_DIR")).resolve()


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
