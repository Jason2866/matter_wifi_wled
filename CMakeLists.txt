cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# esp_matter's CMakeLists.txt calls idf_component_get_property for the
# EXECUTABLE_COMPONENT_NAME, defaulting to "main".  PlatformIO names the
# src/ directory component "src" instead, so we override the variable here.
set(EXECUTABLE_COMPONENT_NAME "src")

project(matter_wifi_wled)

# IDF 5.5.x defaults to -std=gnu++2b (C++23) which triggers a broken
# constexpr-if-with-is_unsigned_v expression in connectedhomeip's
# TypeTraits.h.  Downgrade to gnu++20 (same fix as WLED-MM PR #5456).
idf_build_replace_option_from_property(CXX_COMPILE_OPTIONS "-std=gnu++2b" "-std=gnu++20")
