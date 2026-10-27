# Keep the example source list automatic: newly added UI C/C++ files and
# generated C assets are picked up on the next CMake reconfigure.
file(GLOB_RECURSE FACTORY_UI_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/ui/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/ui/*.cpp"
)
file(GLOB_RECURSE FACTORY_SERVICE_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/app/services/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/app/services/*.cpp"
)
list(FILTER FACTORY_UI_SRCS EXCLUDE REGEX "lv_font_DingTalk_Sans_[0-9]+\\.c$")
list(APPEND FACTORY_UI_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/ui/assets/image/_bot_left_checker_RGB565A8_110x32.c"
    "${CMAKE_CURRENT_LIST_DIR}/ui/assets/image/_bot_right_stripes_RGB565A8_120x32.c"
    "${CMAKE_CURRENT_LIST_DIR}/ui/assets/fonts/lv_font_ShareTechMono1_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/ui/assets/fonts/lv_font_ShareTechMono1_20.c"
    "${CMAKE_CURRENT_LIST_DIR}/ui/assets/fonts/lv_font_ShareTechMono1_24.c"
    "${CMAKE_CURRENT_LIST_DIR}/ui/assets/fonts/lv_font_ShareTechMono1_28.c"
    "${CMAKE_CURRENT_LIST_DIR}/ui/assets/fonts/lv_font_ShareTechMono1_32.c"
    "${CMAKE_CURRENT_LIST_DIR}/ui/assets/fonts/lv_font_ShareTechMono1_50.c"
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
    esp_lcd
    esp_timer
    esp_netif
    freertos
    fatfs
    esp_audio_codec
    esp_driver_jpeg
    ${FACTORY_LVGL_COMPONENT}
    lvgl_page_manager
    peripheral_drivers_lib
    esp32c5_connectivity
    t_panel_p4_standard
)
