file(GLOB_RECURSE FACTORY_UI_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/ui/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/ui/*.cpp"
)
set(FACTORY_SERVICE_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/services/service_context.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/services/wifi_control.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/services/ble_control.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/services/settings_control.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/services/record_audio.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/services/lora_control.cpp"
)
list(APPEND EXAMPLE_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/main.cpp"
    ${FACTORY_UI_SRCS}
    ${FACTORY_SERVICE_SRCS}
)

list(APPEND EXAMPLE_INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}"
)

set(FACTORY_LVGL_COMPONENT "lvgl")
if("$ENV{IDF_COMPONENT_MANAGER}" STREQUAL "0")
    set(FACTORY_LVGL_COMPONENT "lvgl__lvgl")
endif()

list(APPEND EXAMPLE_PRIV_REQUIRES
    esp_app_format
    esp_driver_ppa
    esp_lcd
    esp_timer
    esp_netif
    esp_wifi
    freertos
    fatfs
    esp_audio_codec
    esp_driver_jpeg
    ${FACTORY_LVGL_COMPONENT}
    lvgl_page_manager
    peripheral_drivers_lib
    esp32c5_connectivity
    t_panel_p4
)


