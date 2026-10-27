list(APPEND EXAMPLE_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/main.cpp"
)
list(APPEND EXAMPLE_PRIV_REQUIRES
    esp_lcd
    freertos
    peripheral_drivers_lib
    t_panel_p4_rect
)
