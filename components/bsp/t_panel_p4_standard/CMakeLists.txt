set(BSP_REQUIRES
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_i2s
    esp_driver_cam
    esp_driver_isp
    esp_driver_ledc
    esp_driver_jpeg
    esp_audio_codec
    esp_audio_simple_player
    esp_codec_dev
    esp_driver_sdmmc
    esp_driver_spi
    esp_hw_support
    esp_lcd
    esp_timer
    esp_tinyusb
    fatfs
    freertos
    heap
    jgromes__radiolib
    peripheral_drivers_lib
    sdmmc
    vfs
)

idf_component_register(
    SRCS
        "src/audio.cpp"
        "src/music.cpp"
        "src/power.cpp"
        "src/board.cpp"
        "src/camera.cpp"
        "src/display.cpp"
        "src/lora.cpp"
        "src/radiolib_esp_idf_hal.cpp"
        "src/sd_card.cpp"
        "src/touch.cpp"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "private_include"
    REQUIRES ${BSP_REQUIRES}
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
