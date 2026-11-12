list(APPEND EXAMPLE_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/main.cpp"
)

# The selected BSP variable is available in the top-level configure, while
# this example can also be evaluated from ESP-IDF's component context. Derive
# the component from the cache hardware selection as a fallback so the
# dependency remains correct in both contexts. Keep the name dynamic because
# the top-level configuration scans literal t_panel_p4_* references to detect
# example/BSP mismatches.
list(APPEND EXAMPLE_PRIV_REQUIRES
    esp_lcd
    esp_timer
    freertos
    lvgl
    t_panel_p4
)
