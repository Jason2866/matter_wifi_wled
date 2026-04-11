// matter_gcc14_compat.h — Force-included in every TU via -include.
//
// Originally provided chip::to_underlying as a workaround for GCC 14 + C++23
// two-phase name lookup issues.  Now that the CMakeLists.txt downgrades the
// C++ standard from gnu++2b to gnu++20 (matching WLED-MM PR #5456), and the
// post-script patches TypeTraits.h, the to_underlying definition here is no
// longer needed and would cause a redefinition error.
//
// This header is kept as a placeholder for any future GCC / CHIP SDK compat
// workarounds that need to be force-included.

#pragma once
