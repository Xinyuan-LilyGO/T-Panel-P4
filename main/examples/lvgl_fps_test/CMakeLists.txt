list(APPEND EXAMPLE_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/main.cpp"
)
list(APPEND EXAMPLE_PRIV_REQUIRES
    esp_lcd
    esp_timer
    freertos
    lvgl
    t_panel_p4_standard
)
