// matter_gcc14_compat.h — Force-included in every TU via -include.
//
// GCC 14 two-phase lookup workaround for CHIP SDK
// The CHIP SDK defines chip::to_underlying() in TypeTraits.h and calls it
// without qualification in template code.  GCC 14's stricter two-phase
// name lookup can't find it via ADL because the function lives in the
// 'chip' namespace but the enum argument types are in child namespaces.
// For TUs with the CHIP SDK on their include path we pull in TypeTraits.h
// early; for others this is a harmless no-op.

#pragma once

#ifdef __cplusplus

#if __has_include(<lib/support/TypeTraits.h>)
#include <lib/support/TypeTraits.h>
#endif

#endif // __cplusplus
