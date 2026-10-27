cmake_minimum_required(VERSION 3.16)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/t_panel_p4_config.cmake")

# Keep configuration and generated files isolated per build directory.
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
set(_sdkconfig_defaults "")

# The BSP music implementation is built for every board example and depends
# on these ESP Audio Simple Player defaults. Keep them in the BSP so audio
# examples do not need to duplicate the resampling/channel/bit-depth contract.
set(_bsp_audio_defaults "${T_PANEL_P4_BSP_DEFAULTS}")
if(EXISTS "${_bsp_audio_defaults}" AND
   NOT T_PANEL_P4_EXAMPLE STREQUAL "lcd/ili9882")
    list(APPEND _sdkconfig_defaults "${_bsp_audio_defaults}")
endif()

# LVGL remains an optional feature profile. Loading it only for examples that
# declare lvgl avoids forcing UI memory and scheduler settings onto headless
# audio, camera, storage, and connectivity examples.
set(_example_manifest "${T_PANEL_P4_EXAMPLE_MANIFEST}")
if(EXISTS "${_example_manifest}")
    file(READ "${_example_manifest}" _example_manifest_contents)
    string(FIND "${_example_manifest_contents}" "lvgl/lvgl:" _lvgl_dependency)
    if(NOT _lvgl_dependency EQUAL -1)
        set(_bsp_lvgl_defaults "${T_PANEL_P4_BSP_LVGL_DEFAULTS}")
        list(APPEND _sdkconfig_defaults "${_bsp_lvgl_defaults}")
    endif()
endif()
set(_example_defaults "${T_PANEL_P4_EXAMPLE_DEFAULTS}")
if(EXISTS "${_example_defaults}")
    list(APPEND _sdkconfig_defaults "${_example_defaults}")
endif()
if(_sdkconfig_defaults)
    set(SDKCONFIG_DEFAULTS "${_sdkconfig_defaults}")
endif()
set(IDF_TARGET "esp32p4" CACHE STRING "ESP-IDF target")

# Prevent accidental reuse of one build directory for another hardware/example
# combination.
set(_variant_file "${CMAKE_BINARY_DIR}/t_panel_p4.example")
set(_build_variant "${T_PANEL_P4_HARDWARE_VERSION}:${T_PANEL_P4_EXAMPLE}")
if(EXISTS "${_variant_file}")
    file(READ "${_variant_file}" _configured_variant)
    string(STRIP "${_configured_variant}" _configured_variant)
    if(_configured_variant STREQUAL T_PANEL_P4_EXAMPLE)
        # Older project versions recorded only the example. Upgrade that
        # marker in place once the hardware selection is known.
        file(WRITE "${_variant_file}" "${_build_variant}\n")
    elseif(NOT _configured_variant STREQUAL _build_variant)
        message(FATAL_ERROR
            "Build directory is configured for '${_configured_variant}', not "
            "'${_build_variant}'. Use a separate build directory.")
    endif()
else()
    file(WRITE "${_variant_file}" "${_build_variant}\n")
endif()

# Materialize the selected example as the build directory's main component.
# ESP-IDF scans component dependencies in a separate CMake process which does
# not inherit arbitrary project cache variables.
set(T_PANEL_P4_MAIN_CMAKE
    "${CMAKE_CURRENT_LIST_DIR}/main/CMakeLists.txt")
set(_generated_main_dir "${CMAKE_BINARY_DIR}/selected_example/main")
file(MAKE_DIRECTORY "${_generated_main_dir}")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/cmake/selected_example/CMakeLists.txt.in"
    "${_generated_main_dir}/CMakeLists.txt"
    @ONLY
)
if(EXISTS "${_example_manifest}")
    configure_file(
        "${_example_manifest}"
        "${_generated_main_dir}/idf_component.yml"
        COPYONLY
    )
endif()
if(EXISTS "${T_PANEL_P4_EXAMPLE_DIR}/Kconfig")
    configure_file(
        "${T_PANEL_P4_EXAMPLE_DIR}/Kconfig"
        "${_generated_main_dir}/Kconfig"
        COPYONLY
    )
endif()

set(COMPONENT_DIRS
    "$ENV{IDF_PATH}/components"
    "${_generated_main_dir}"
    "${T_PANEL_P4_BSP_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/components/lvgl_page_manager"
    "${CMAKE_CURRENT_LIST_DIR}/components/peripheral_drivers_lib"
)

if(T_PANEL_P4_EXAMPLE STREQUAL "sdio_esp32c5" OR
   T_PANEL_P4_EXAMPLE STREQUAL "factory_standard")
    list(APPEND COMPONENT_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/components/connectivity/esp32c5_connectivity"
    )
    # esp_wifi_remote 1.6.x names its IDF-specific Kconfig by major.minor.
    # Set this before project.cmake discovers and evaluates managed components.
    set(ENV{ESP_IDF_VERSION} "5.5")
endif()

if(T_PANEL_P4_EXAMPLE STREQUAL "lcd/ili9882")
    set(ENV{T_PANEL_P4_RECT_LCD_ONLY} "1")
    # The RECT LCD example does not use the standard BSP, LoRa, or audio
    # dependency graph. Keep those components out of dependency discovery.
    set(EXCLUDE_COMPONENTS t_panel_p4_standard jgromes__radiolib)
endif()

# Permit reproducible offline builds after managed dependencies are present.
if("$ENV{IDF_COMPONENT_MANAGER}" STREQUAL "0")
    if(NOT T_PANEL_P4_EXAMPLE STREQUAL "lcd/ili9882")
        list(APPEND COMPONENT_DIRS
            "${CMAKE_CURRENT_LIST_DIR}/managed_components/jgromes__radiolib"
            "${CMAKE_CURRENT_LIST_DIR}/managed_components/lvgl__lvgl"
        )
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(MINIMAL_BUILD ON)
string(REPLACE "/" "-" _project_example "${T_PANEL_P4_EXAMPLE}")
project(T-Panel-P4-${_project_example})

message(STATUS "T-Panel-P4 example: ${T_PANEL_P4_EXAMPLE}")
message(STATUS "T-Panel-P4 build directory: ${CMAKE_BINARY_DIR}")

