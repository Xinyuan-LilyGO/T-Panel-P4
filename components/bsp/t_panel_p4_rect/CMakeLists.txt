set(BSP_REQUIRES
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_i2s
    esp_driver_cam
    esp_driver_isp
    esp_driver_ledc
    esp_driver_jpeg
    esp_audio_simple_player
    esp_driver_sdmmc
    esp_hw_support
    esp_lcd
    esp_timer
    fatfs
    freertos
    heap
    peripheral_drivers_lib
    sdmmc
    vfs
)

set(_rect_lcd_only "$ENV{T_PANEL_P4_RECT_LCD_ONLY}")

if(NOT _rect_lcd_only)
    if("$ENV{IDF_COMPONENT_MANAGER}" STREQUAL "0")
        list(APPEND BSP_REQUIRES
            espressif__esp_tinyusb
            espressif__tinyusb)
    else()
        list(APPEND BSP_REQUIRES esp_tinyusb)
    endif()
endif()

set(BSP_SRCS
    "src/audio.cpp"
    "src/power.cpp"
    "src/board.cpp"
    "src/camera.cpp"
    "src/display.cpp"
    "src/sd_card.cpp"
    "src/touch.cpp"
)

if(NOT _rect_lcd_only)
    list(APPEND BSP_SRCS
        "src/music.cpp"
        "src/recording.cpp"
    )
endif()

idf_component_register(
    SRCS ${BSP_SRCS}
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "private_include"
    REQUIRES ${BSP_REQUIRES}
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

if(_rect_lcd_only)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        T_PANEL_P4_RECT_NO_AUDIO_FEATURES=1)
endif()
