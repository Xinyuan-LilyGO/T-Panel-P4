list(APPEND EXAMPLE_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/main.cpp"
)
list(APPEND EXAMPLE_PRIV_REQUIRES
    freertos
    t_panel_p4_standard
)
list(APPEND EXAMPLE_EMBED_FILES
    "${CMAKE_CURRENT_LIST_DIR}/sample-3s.wav"
)
