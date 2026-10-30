list(APPEND EXAMPLE_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/main.cpp"
)

list(APPEND EXAMPLE_PRIV_REQUIRES
    freertos
    peripheral_drivers_lib
)
