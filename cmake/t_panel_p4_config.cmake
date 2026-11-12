# Select the hardware profile of the shared BSP and the example to build.
#
# This file is included by the project CMakeLists.txt before ESP-IDF's
# project.cmake. Keep all paths absolute: ESP-IDF evaluates generated
# component CMake files from a separate CMake process.

set(T_PANEL_P4_PROJECT_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

# T_PANEL_P4_BOARD is kept as a compatibility alias for existing build
# scripts. New build scripts should use T_PANEL_P4_HARDWARE_VERSION.
if(DEFINED T_PANEL_P4_BOARD AND DEFINED T_PANEL_P4_HARDWARE_VERSION AND
   NOT T_PANEL_P4_BOARD STREQUAL T_PANEL_P4_HARDWARE_VERSION)
    message(FATAL_ERROR
        "T_PANEL_P4_BOARD and T_PANEL_P4_HARDWARE_VERSION select different "
        "hardware versions")
endif()
if(NOT DEFINED T_PANEL_P4_HARDWARE_VERSION)
    if(DEFINED T_PANEL_P4_BOARD)
        set(_default_hardware_version "${T_PANEL_P4_BOARD}")
    else()
        set(_default_hardware_version "standard")
    endif()
    set(T_PANEL_P4_HARDWARE_VERSION "${_default_hardware_version}")
endif()
if(NOT DEFINED CACHE{T_PANEL_P4_HARDWARE_VERSION})
    set(T_PANEL_P4_HARDWARE_VERSION "${T_PANEL_P4_HARDWARE_VERSION}" CACHE STRING
        "T-Panel-P4 hardware version")
endif()
set_property(CACHE T_PANEL_P4_HARDWARE_VERSION PROPERTY STRINGS standard rect round)

string(TOLOWER "${T_PANEL_P4_HARDWARE_VERSION}" T_PANEL_P4_HARDWARE_VERSION)
set(_supported_hardware_versions standard rect round)
list(FIND _supported_hardware_versions
    "${T_PANEL_P4_HARDWARE_VERSION}" _hardware_index)
if(_hardware_index EQUAL -1)
    message(FATAL_ERROR
        "Unsupported T_PANEL_P4_HARDWARE_VERSION='${T_PANEL_P4_HARDWARE_VERSION}'. "
        "Choose one of: ${_supported_hardware_versions}")
endif()

if(T_PANEL_P4_HARDWARE_VERSION STREQUAL "standard")
    set(T_PANEL_P4_HARDWARE_VERSION_VALUE 1)
    set(T_PANEL_P4_HAS_ESP32C5 TRUE)
    set(T_PANEL_P4_HAS_LORA TRUE)
elseif(T_PANEL_P4_HARDWARE_VERSION STREQUAL "rect")
    set(T_PANEL_P4_HARDWARE_VERSION_VALUE 2)
    set(T_PANEL_P4_HAS_ESP32C5 FALSE)
    set(T_PANEL_P4_HAS_LORA FALSE)
elseif(T_PANEL_P4_HARDWARE_VERSION STREQUAL "round")
    set(T_PANEL_P4_HARDWARE_VERSION_VALUE 3)
    set(T_PANEL_P4_HAS_ESP32C5 TRUE)
    set(T_PANEL_P4_HAS_LORA TRUE)
endif()

# The CMake variable above is only available while configuring the top-level
# project. Export an integer enum through ESP-IDF's global compile definitions
# so shared C/C++ code can select board-specific behavior with preprocessor
# checks. The symbolic values are exported for readable comparisons.
set(T_PANEL_P4_HARDWARE_COMPILE_DEFINITIONS
    "T_PANEL_P4_HARDWARE_VERSION=${T_PANEL_P4_HARDWARE_VERSION_VALUE}"
    "T_PANEL_P4_HARDWARE_VERSION_STANDARD=1"
    "T_PANEL_P4_HARDWARE_VERSION_RECT=2"
    "T_PANEL_P4_HARDWARE_VERSION_ROUND=3")

set(T_PANEL_P4_BSP_DIR
    "${T_PANEL_P4_PROJECT_DIR}/components/bsp/t_panel_p4")
set(T_PANEL_P4_BSP_PROFILE_DIR
    "${T_PANEL_P4_BSP_DIR}/profiles/${T_PANEL_P4_HARDWARE_VERSION}")
set(T_PANEL_P4_BSP_DEFAULTS
    "${T_PANEL_P4_BSP_PROFILE_DIR}/sdkconfig.defaults")
set(T_PANEL_P4_BSP_LVGL_DEFAULTS
    "${T_PANEL_P4_BSP_PROFILE_DIR}/sdkconfig.lvgl.defaults")

# Keep the old board variable available to existing example CMake files and
# build scripts without creating a second cache setting to maintain.
set(T_PANEL_P4_BOARD "${T_PANEL_P4_HARDWARE_VERSION}")

if(NOT DEFINED T_PANEL_P4_EXAMPLE)
    set(T_PANEL_P4_EXAMPLE "factory_standard")
endif()
if(NOT DEFINED CACHE{T_PANEL_P4_EXAMPLE})
    set(T_PANEL_P4_EXAMPLE "${T_PANEL_P4_EXAMPLE}" CACHE STRING
        "Example directory under main/examples to build")
endif()
set_property(CACHE T_PANEL_P4_EXAMPLE PROPERTY STRINGS
    factory_standard
    factory_round
    axp517
    xl9555
    camera
    audio/loopback
    audio/play_wav
    audio/play_sd_mp3
    lcd_touch
    lr2021
    lvgl_fps_test
    lvgl_lcd_touch
    rect_lowpower_test
    sd
    sdio_esp32c5
    usb_msc)

string(TOLOWER "${T_PANEL_P4_EXAMPLE}" T_PANEL_P4_EXAMPLE)
string(REPLACE "\\" "/" T_PANEL_P4_EXAMPLE
    "${T_PANEL_P4_EXAMPLE}")
if(NOT T_PANEL_P4_EXAMPLE MATCHES "^[a-z0-9][a-z0-9_/-]*$")
    message(FATAL_ERROR
        "Invalid T_PANEL_P4_EXAMPLE='${T_PANEL_P4_EXAMPLE}'")
endif()

set(T_PANEL_P4_EXAMPLE_DIR
    "${T_PANEL_P4_PROJECT_DIR}/main/examples/${T_PANEL_P4_EXAMPLE}")
set(T_PANEL_P4_EXAMPLE_CMAKE
    "${T_PANEL_P4_EXAMPLE_DIR}/CMakeLists.txt")
set(T_PANEL_P4_EXAMPLE_MANIFEST
    "${T_PANEL_P4_EXAMPLE_DIR}/idf_component.yml")
set(T_PANEL_P4_EXAMPLE_DEFAULTS
    "${T_PANEL_P4_EXAMPLE_DIR}/sdkconfig.defaults")
if(NOT EXISTS "${T_PANEL_P4_EXAMPLE_CMAKE}")
    message(FATAL_ERROR
        "Unknown example '${T_PANEL_P4_EXAMPLE}'. Expected: "
        "${T_PANEL_P4_EXAMPLE_CMAKE}")
endif()

# Reject examples whose hardware feature is physically absent. This gives a
# direct configuration error instead of a later unresolved-component error.
set(_examples_require_esp32c5
    sdio_esp32c5
    factory_standard
    factory_round)
list(FIND _examples_require_esp32c5 "${T_PANEL_P4_EXAMPLE}"
    _example_requires_esp32c5)
if(_example_requires_esp32c5 GREATER -1 AND NOT T_PANEL_P4_HAS_ESP32C5)
    message(FATAL_ERROR
        "Example '${T_PANEL_P4_EXAMPLE}' requires the ESP32-C5 slave, but "
        "hardware version '${T_PANEL_P4_HARDWARE_VERSION}' has no ESP32-C5.")
endif()

if(T_PANEL_P4_EXAMPLE STREQUAL "lr2021" AND NOT T_PANEL_P4_HAS_LORA)
    message(FATAL_ERROR
        "Example 'lr2021' requires the LoRa module, but hardware version "
        "'${T_PANEL_P4_HARDWARE_VERSION}' has no LoRa module.")
endif()

# Factory UIs and the low-power example still need matching hardware.
# Shared examples such as axp517 and lcd_touch work on all profiles.
set(_standard_only factory_standard)
set(_round_only factory_round)
set(_rect_only rect_lowpower_test)
foreach(_profile IN LISTS _supported_hardware_versions)
    if(T_PANEL_P4_EXAMPLE IN_LIST _${_profile}_only AND
       NOT T_PANEL_P4_HARDWARE_VERSION STREQUAL _profile)
        message(FATAL_ERROR
            "Example '${T_PANEL_P4_EXAMPLE}' requires '${_profile}' hardware.")
    endif()
endforeach()
if(T_PANEL_P4_EXAMPLE STREQUAL "xl9555" AND
   T_PANEL_P4_HARDWARE_VERSION STREQUAL "rect")
    message(FATAL_ERROR "Example 'xl9555' requires the XL9555 on standard or round hardware.")
endif()

message(STATUS "T-Panel-P4 hardware version: ${T_PANEL_P4_HARDWARE_VERSION}")
message(STATUS "T-Panel-P4 BSP: ${T_PANEL_P4_BSP_DIR}")
message(STATUS "T-Panel-P4 example: ${T_PANEL_P4_EXAMPLE_DIR}")
