# Select the board hardware, its BSP component, and the example to build.
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
    set(T_PANEL_P4_BSP_COMPONENT "t_panel_p4_standard")
elseif(T_PANEL_P4_HARDWARE_VERSION STREQUAL "rect")
    set(T_PANEL_P4_BSP_COMPONENT "t_panel_p4_rect")
elseif(T_PANEL_P4_HARDWARE_VERSION STREQUAL "round")
    set(T_PANEL_P4_BSP_COMPONENT "t_panel_p4_round")
endif()

set(T_PANEL_P4_BSP_DIR
    "${T_PANEL_P4_PROJECT_DIR}/components/bsp/${T_PANEL_P4_BSP_COMPONENT}")
set(T_PANEL_P4_BSP_CMAKE_FILE
    "${T_PANEL_P4_BSP_DIR}/CMakeLists.txt")
set(T_PANEL_P4_BSP_DEFAULTS
    "${T_PANEL_P4_BSP_DIR}/sdkconfig.defaults")
set(T_PANEL_P4_BSP_LVGL_DEFAULTS
    "${T_PANEL_P4_BSP_DIR}/sdkconfig.lvgl.defaults")
if(NOT EXISTS "${T_PANEL_P4_BSP_CMAKE_FILE}")
    message(FATAL_ERROR
        "BSP CMake file does not exist for hardware version "
        "'${T_PANEL_P4_HARDWARE_VERSION}': ${T_PANEL_P4_BSP_CMAKE_FILE}")
endif()

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
    axp517
    xl9555
    camera
    audio/loopback
    audio/play_wav
    audio/play_sd_mp3
    lcd/ili9882
    lcd/jd9365
    lr2021
    lvgl_fps_test
    lvgl_lcd_touch
    rect_lowpower_test
    sd
    sdio_esp32c5
    touch/jadard
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

# An example declares its BSP in its own CMake file. Catch a board/example
# mismatch while configuring instead of failing later during component
# dependency resolution.
file(READ "${T_PANEL_P4_EXAMPLE_CMAKE}" _example_cmake_contents)
string(REGEX MATCHALL "t_panel_p4_[a-z0-9_]+" _example_bsp_components
    "${_example_cmake_contents}")
list(REMOVE_DUPLICATES _example_bsp_components)
foreach(_example_bsp_component IN LISTS _example_bsp_components)
    # The round BSP intentionally keeps the standard BSP's public header and
    # namespace, so existing standard examples can run on the round board.
    set(_example_bsp_compatible FALSE)
    if(_example_bsp_component STREQUAL T_PANEL_P4_BSP_COMPONENT)
        set(_example_bsp_compatible TRUE)
    elseif(T_PANEL_P4_HARDWARE_VERSION STREQUAL "round" AND
           _example_bsp_component STREQUAL "t_panel_p4_standard")
        set(_example_bsp_compatible TRUE)
    endif()
    if(NOT _example_bsp_compatible)
        message(FATAL_ERROR
            "Example '${T_PANEL_P4_EXAMPLE}' requires BSP "
            "'${_example_bsp_component}', but hardware version "
            "'${T_PANEL_P4_HARDWARE_VERSION}' selects "
            "'${T_PANEL_P4_BSP_COMPONENT}'.")
    endif()
endforeach()

message(STATUS "T-Panel-P4 hardware version: ${T_PANEL_P4_HARDWARE_VERSION}")
message(STATUS "T-Panel-P4 BSP: ${T_PANEL_P4_BSP_CMAKE_FILE}")
message(STATUS "T-Panel-P4 example: ${T_PANEL_P4_EXAMPLE_DIR}")
