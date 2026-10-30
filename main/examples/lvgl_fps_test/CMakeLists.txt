list(APPEND EXAMPLE_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/main.cpp"
)

# The selected BSP variable is available in the top-level configure, while
# this example can also be evaluated from ESP-IDF's component context. Derive
# the component from the cache hardware selection as a fallback so the
# dependency remains correct in both contexts. Keep the name dynamic because
# the top-level configuration scans literal t_panel_p4_* references to detect
# example/BSP mismatches.
set(_lvgl_fps_bsp_component "${T_PANEL_P4_BSP_COMPONENT}")
if(NOT _lvgl_fps_bsp_component)
    set(_lvgl_fps_bsp_component
        "t_panel_p4_${T_PANEL_P4_HARDWARE_VERSION}")
endif()

list(APPEND EXAMPLE_PRIV_REQUIRES
    esp_lcd
    esp_timer
    freertos
    lvgl
    "${_lvgl_fps_bsp_component}"
)
