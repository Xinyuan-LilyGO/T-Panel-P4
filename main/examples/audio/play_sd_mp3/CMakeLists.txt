list(APPEND EXAMPLE_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/main.cpp"
)

list(APPEND EXAMPLE_PRIV_REQUIRES
    freertos
    esp_audio_simple_player
    t_panel_p4_standard
)
