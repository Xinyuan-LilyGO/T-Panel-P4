idf_component_register(
    SRCS
        "src/esp32c5_connectivity.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        bt
        esp_event
        esp_hosted
        esp_netif
        esp_wifi
        esp_wifi_remote
        freertos
        nvs_flash
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
