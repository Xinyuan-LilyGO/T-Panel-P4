set(BSP_SRCS
    src/audio.cpp
    src/board.cpp
    src/camera.cpp
    src/display.cpp
    src/music.cpp
    src/power.cpp
    src/recording.cpp
    src/sd_card.cpp
    src/touch.cpp
)

set(BSP_REQUIRES
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_i2s
    esp_driver_cam
    esp_driver_isp
    esp_driver_ledc
    esp_driver_jpeg
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

# ESP-IDF discovers dependencies in a separate CMake process.
set(_hardware "$ENV{T_PANEL_P4_BSP_PROFILE}")
if(_hardware STREQUAL "standard" OR _hardware STREQUAL "round")
    list(APPEND BSP_SRCS src/lora.cpp src/radiolib_esp_idf_hal.cpp)
    list(APPEND BSP_REQUIRES esp_driver_spi)
elseif(NOT _hardware STREQUAL "rect")
    message(FATAL_ERROR "Unknown T-Panel-P4 BSP profile: '${_hardware}'")
endif()

idf_component_register(
    SRCS ${BSP_SRCS}
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS private_include
    REQUIRES ${BSP_REQUIRES}
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

