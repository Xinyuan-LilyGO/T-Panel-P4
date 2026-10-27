#include "service_context.hpp"

namespace
{
factory::services::Context s_context{};
}

namespace factory::services
{
void set_context(bsp::t_panel_p4_standard::Board &board,
                 connectivity::Esp32c5Connectivity &connectivity,
                 connectivity::Esp32c5WifiStationConfig *wifi_config,
                 bool *wifi_config_valid,
                 esp_err_t *connectivity_result)
{
    s_context.board = &board;
    s_context.connectivity = &connectivity;
    s_context.wifi_config = wifi_config;
    s_context.wifi_config_valid = wifi_config_valid;
    s_context.connectivity_result = connectivity_result;
}

const Context &context()
{
    return s_context;
}

bsp::t_panel_p4_standard::Board *board()
{
    return s_context.board;
}

connectivity::Esp32c5Connectivity *connectivity()
{
    return s_context.connectivity;
}
}
