list(APPEND EXAMPLE_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/main.cpp"
)

set(bsp_component "${T_PANEL_P4_BSP_COMPONENT}")
if(NOT bsp_component)
    set(bsp_component
        "t_panel_p4_${T_PANEL_P4_HARDWARE_VERSION}")
endif()

list(APPEND EXAMPLE_PRIV_REQUIRES
    esp32c5_connectivity
    esp_netif
    ${bsp_component}
)
