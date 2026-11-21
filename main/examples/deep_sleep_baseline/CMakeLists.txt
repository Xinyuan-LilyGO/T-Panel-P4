list(APPEND EXAMPLE_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/main.cpp"
)
list(APPEND EXAMPLE_PRIV_REQUIRES
    esp_driver_gpio
    esp_driver_i2c
    esp_hw_support
    esp_psram
    peripheral_drivers_lib
    t_panel_p4
    esp_system
    freertos
)
